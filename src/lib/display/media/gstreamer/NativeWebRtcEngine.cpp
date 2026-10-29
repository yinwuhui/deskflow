// SPDX-License-Identifier: MIT
#include "NativeWebRtcEngine.h"
#include "GStreamerBuildManifest.h"
#include "H264CodecEvidence.h"
#include "display/supervision/WorkerProcess.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <atomic>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/pbutils/codec-utils.h>
#include <gst/rtp/rtp.h>
#include <gst/video/video.h>
#include <gst/webrtc/webrtc.h>
#include <map>
#include <mutex>
#include <nice/agent.h>
namespace deskflow::display::infra {
H264CodecEvidence inspectH264CodecEvidence(const GstCaps *caps)
{
  H264CodecEvidence result;
  if (!caps || !gst_caps_is_fixed(caps) || gst_caps_get_size(caps) != 1)
    return result;
  const auto *structure = gst_caps_get_structure(caps, 0);
  if (!gst_structure_has_name(structure, "video/x-h264") ||
      g_strcmp0(gst_structure_get_string(structure, "stream-format"), "avc") ||
      g_strcmp0(gst_structure_get_string(structure, "alignment"), "au"))
    return result;
  const auto *value = gst_structure_get_value(structure, "codec_data");
  if (!value) {
    result.state = H264Evidence::Pending;
    return result;
  }
  if (!GST_VALUE_HOLDS_BUFFER(value))
    return result;
  auto *buffer = gst_value_get_buffer(value);
  const auto bytes = buffer ? gst_buffer_get_size(buffer) : 0;
  if (bytes < 7 || bytes > 65536)
    return result;
  GstMapInfo map = GST_MAP_INFO_INIT;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
    return result;
  const bool parsed = gst_codec_utils_h264_get_profile_flags_level(
      map.data, guint(map.size), &result.profile, &result.flags, &result.level
  );
  gst_buffer_unmap(buffer, &map);
  const guint8 header[] = {result.profile, result.flags, result.level};
  if (parsed && !g_strcmp0(gst_codec_utils_h264_get_profile(header, sizeof(header)), "constrained-baseline") &&
      gst_codec_utils_h264_get_level(header, sizeof(header)))
    result.state = H264Evidence::ConstrainedBaseline;
  return result;
}

namespace {
using RtpKey = std::pair<uint32_t, uint32_t>;
struct State;
using Shared = std::shared_ptr<State>;
struct Connection
{
  GObject *object;
  gulong id;
};
struct Probe
{
  GstPad *pad;
  gulong id;
};
struct Pending
{
  FrameTicket ticket;
  RtpKey key;
};
struct Input
{
  GstClockTime pts;
  NativeFrameCorrelation correlation;
  bool emitted = false;
};
struct State
{
  NativeWebRtcConfiguration config;
  std::atomic<bool> closing{false}, nativeStopped{true}, pairApproved{false}, profile{false}, refreshPending{false},
      signallerReady{false};
  std::atomic<const char *> failure{nullptr};
  std::atomic<uint64_t> inputFrames{0}, decodedFrames{0}, gccUpdates{0}, encoderUpdates{0}, deniedPackets{0};
  std::atomic<uint32_t> estimated{0}, encoderRate{0};
  std::atomic<uint64_t> compatibleProfileLabels{0};
  std::mutex mutex, connectionsMutex, parserMutex;
  std::vector<Connection> connections;
  std::vector<Probe> probes;
  std::deque<WorkerMediaSignal> outboundSignals, startupSignals;
  qsizetype startupSignalBytes = 0;
  qsizetype signalBytes = 0;
  std::deque<NativeFrameCorrelation> outboundMaps;
  std::map<RtpKey, NativeFrameCorrelation> inboundMaps;
  std::deque<RtpKey> mapOrder;
  std::map<GstClockTime, RtpKey> decodeTimes;
  std::deque<Pending> pending;
  std::deque<Input> inputs;
  std::shared_ptr<FrameBudget> budget = std::make_shared<FrameBudget>(3, 32 * 1024 * 1024);
  std::shared_ptr<FramePool> output;
  std::shared_ptr<CpuFrameLease> lastInput;
  GstElement *pipeline = nullptr, *source = nullptr, *signallingElement = nullptr, *encoder = nullptr;
  GObject *signaller = nullptr;
  NiceAgent *parser = nullptr;
  guint parserStream = 0;
  bool prepared = false, started = false, sessionStarted = false;
  uint64_t inputSequence = 0, inputCapture = 0, mediaSequence = 0, lastDecoded = 0;
  GstClockTime lastPts = GST_CLOCK_TIME_NONE;
  QString remoteSdpDigest, pipelineDiagnostic;
  void fail(const char *why)
  {
    const char *empty = nullptr;
    failure.compare_exchange_strong(empty, why);
    pairApproved.store(false);
  }
  ~State()
  {
    if (parser)
      g_object_unref(parser);
    if (encoder)
      gst_object_unref(encoder);
  }
};
std::mutex runtimeMutex;
bool runtimeReady = false;
QString runtimeRoot;
std::unique_ptr<QTemporaryDir> registryDirectory;
std::atomic<bool> poisoned{false};
Shared state(gpointer data)
{
  return *static_cast<Shared *>(data);
}
void connect(const Shared &s, GObject *object, const char *name, GCallback callback)
{
  std::lock_guard lock(s->connectionsMutex);
  if (s->closing.load())
    return;
  if (s->connections.size() >= 256) {
    s->fail("NativeSignalBudgetExceeded");
    return;
  }
  guint signalId = 0;
  GQuark detail = 0;
  if (!g_signal_parse_name(name, G_OBJECT_TYPE(object), &signalId, &detail, TRUE)) {
    s->fail("NativeSignalUnsupported");
    return;
  }
  auto *holder = new Shared(s);
  auto id = g_signal_connect_data(
      object, name, callback, holder, [](gpointer p, GClosure *) { delete static_cast<Shared *>(p); }, GConnectFlags(0)
  );
  s->connections.push_back({G_OBJECT(g_object_ref(object)), id});
}
void probe(const Shared &s, GstPad *pad, GstPadProbeType type, GstPadProbeCallback callback)
{
  if (!pad) {
    s->fail("MediaPadMissing");
    return;
  }
  std::lock_guard lock(s->connectionsMutex);
  if (!s->closing.load()) {
    auto id =
        gst_pad_add_probe(pad, type, callback, new Shared(s), [](gpointer p) { delete static_cast<Shared *>(p); });
    s->probes.push_back({GST_PAD(gst_object_ref(pad)), id});
  }
  gst_object_unref(pad);
}
bool address(const NiceAddress &value, const QStringList &allowed)
{
  if (!nice_address_is_valid(&value))
    return false;
  char text[NICE_ADDRESS_STRING_LEN]{};
  nice_address_to_string(&value, text);
  const QHostAddress candidate(QString::fromLatin1(text));
  for (const auto &item : allowed)
    if (QHostAddress(item) == candidate)
      return true;
  return false;
}
bool candidate(const Shared &s, QString value, bool local = false)
{
  if (value.toUtf8().size() > 1024)
    return false;
  if (!value.startsWith("a="))
    value.prepend("a=");
  auto bytes = value.toUtf8();
  std::lock_guard lock(s->parserMutex);
  NiceCandidate *parsed = nice_agent_parse_remote_candidate_sdp(s->parser, s->parserStream, bytes.constData());
  if (!parsed)
    return false;
  const bool safe =
      parsed->type == NICE_CANDIDATE_TYPE_HOST && parsed->transport == NICE_CANDIDATE_TRANSPORT_UDP &&
      parsed->component_id == 1 && nice_address_get_port(&parsed->addr) > 0 &&
      address(parsed->addr, local ? QStringList{s->config.localAddress} : s->config.preparation.allowedPeerAddresses);
  nice_candidate_free(parsed);
  return safe;
}
void enqueue(const Shared &s, WorkerMediaSignal signal)
{
  if (s->closing.load())
    return;
  if (!validMediaSignal(signal)) {
    s->fail("MediaSignalInvalid");
    return;
  }
  std::lock_guard lock(s->mutex);
  const auto bytes = signal.description.toUtf8().size() + signal.candidate.toUtf8().size() + signal.mid.toUtf8().size();
  if (s->outboundSignals.size() >= 72 || bytes > 256 * 1024 - s->signalBytes) {
    s->fail("MediaSignalQueueFull");
    return;
  }
  s->signalBytes += bytes;
  s->outboundSignals.push_back(std::move(signal));
}
gboolean startHandled(GObject *, gpointer data)
{
  state(data)->signallerReady.store(true);
  return TRUE;
}
gboolean handled(GObject *, gpointer)
{
  return TRUE;
}
gboolean endHandled(GObject *, const gchar *, gpointer)
{
  return TRUE;
}
gboolean sendSdp(GObject *, const gchar *, GstWebRTCSessionDescription *description, gpointer data)
{
  auto s = state(data);
  if (s->closing.load())
    return TRUE;
  gchar *text = gst_sdp_message_as_text(description->sdp);
  WorkerMediaSignal signal;
  signal.kind = description->type == GST_WEBRTC_SDP_TYPE_OFFER ? WebRtcSignalKind::Offer : WebRtcSignalKind::Answer;
  signal.description = QString::fromUtf8(text);
  g_free(text);
  enqueue(s, std::move(signal));
  return TRUE;
}
gboolean sendIce(GObject *, const gchar *, const gchar *value, guint line, const gchar *mid, gpointer data)
{
  auto s = state(data);
  if (s->closing.load() || !value || !*value)
    return TRUE;
  WorkerMediaSignal signal;
  signal.kind = WebRtcSignalKind::Ice;
  signal.candidate = QString::fromUtf8(value);
  signal.mediaLine = line;
  signal.mid = QString::fromUtf8(mid ? mid : "");
  if (candidate(s, signal.candidate, true))
    enqueue(s, std::move(signal));
  return TRUE;
}
void selectedPair(NiceAgent *, guint, guint component, NiceCandidate *local, NiceCandidate *remote, gpointer data)
{
  auto s = state(data);
  if (s->closing.load() || component != 1)
    return;
  const bool approved = local && remote && local->type == NICE_CANDIDATE_TYPE_HOST &&
                        remote->type == NICE_CANDIDATE_TYPE_HOST && local->transport == NICE_CANDIDATE_TRANSPORT_UDP &&
                        remote->transport == NICE_CANDIDATE_TRANSPORT_UDP &&
                        address(local->addr, {s->config.localAddress}) &&
                        address(remote->addr, s->config.preparation.allowedPeerAddresses);
  if (!approved) {
    s->fail("IceSelectedPairDenied");
    return;
  }
  s->pairApproved.store(true);
  s->refreshPending.store(true);
}
void webrtcReady(GObject *, const gchar *, GstElement *webrtc, gpointer data)
{
  auto s = state(data);
  if (s->closing.load())
    return;
  g_object_set(webrtc, "stun-server", nullptr, nullptr);
  gchar *turn = nullptr;
  g_object_get(webrtc, "turn-server", &turn, nullptr);
  if (turn && *turn)
    s->fail("UnexpectedTurnServer");
  g_free(turn);
  GObject *ice = nullptr, *agent = nullptr;
  g_object_get(webrtc, "ice-agent", &ice, nullptr);
  if (ice && g_object_class_find_property(G_OBJECT_GET_CLASS(ice), "agent"))
    g_object_get(ice, "agent", &agent, nullptr);
  if (!agent || !NICE_IS_AGENT(agent) || !g_object_class_find_property(G_OBJECT_GET_CLASS(agent), "upnp") ||
      !g_object_class_find_property(G_OBJECT_GET_CLASS(agent), "ice-tcp"))
    s->fail("IcePolicyUnsupported");
  else {
    g_object_set(agent, "upnp", FALSE, "ice-tcp", FALSE, nullptr);
    NiceAddress local;
    nice_address_init(&local);
    const auto text = s->config.localAddress.toLatin1();
    if (!nice_address_set_from_string(&local, text.constData()) ||
        !nice_agent_add_local_address(NICE_AGENT(agent), &local))
      s->fail("IceLocalBindingFailed");
    else
      connect(s, agent, "new-selected-pair-full", G_CALLBACK(selectedPair));
  }
  if (agent)
    g_object_unref(agent);
  if (ice)
    g_object_unref(ice);
}
void rateChanged(GObject *object, GParamSpec *, gpointer data)
{
  auto s = state(data);
  guint value = 0;
  g_object_get(object, "estimated-bitrate", &value, nullptr);
  s->estimated.store(value);
  ++s->gccUpdates;
}
void encoderRate(GObject *object, GParamSpec *, gpointer data)
{
  auto s = state(data);
  guint value = 0;
  g_object_get(object, "bitrate", &value, nullptr);
  s->encoderRate.store(value);
  ++s->encoderUpdates;
}
GstPadProbeReturn encoderProbe(GstPad *, GstPadProbeInfo *info, gpointer data)
{
  auto s = state(data);
  if (s->closing.load())
    return GST_PAD_PROBE_DROP;
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
      GstCaps *caps = nullptr;
      gst_event_parse_caps(event, &caps);
      const auto evidence = inspectH264CodecEvidence(caps);
      s->profile.store(evidence.state == H264Evidence::ConstrainedBaseline);
      if (evidence.state == H264Evidence::ConstrainedBaseline &&
          g_strcmp0(gst_structure_get_string(gst_caps_get_structure(caps, 0), "profile"), "constrained-baseline"))
        ++s->compatibleProfileLabels;
      if (evidence.state == H264Evidence::Rejected) {
        std::lock_guard lock(s->mutex);
        s->pipelineDiagnostic =
            QString("h264-header/%1/%2/%3").arg(evidence.profile).arg(evidence.flags).arg(evidence.level);
        s->fail("EncodedProfileMismatch");
      }
    }
  }
  if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) &&
      gst_buffer_get_size(GST_PAD_PROBE_INFO_BUFFER(info)) > s->config.preparation.encodedQueueBytes) {
    s->fail("EncodedFrameBudgetExceeded");
    return GST_PAD_PROBE_DROP;
  }
  return GST_PAD_PROBE_OK;
}
GstPadProbeReturn inspectOutgoing(const Shared &s, GstPad *pad, GstBuffer *buffer)
{
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  if (!gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) {
    s->fail("RtpInspectionFailed");
    return GST_PAD_PROBE_DROP;
  }
  RtpKey key{gst_rtp_buffer_get_ssrc(&rtp), gst_rtp_buffer_get_timestamp(&rtp)};
  gst_rtp_buffer_unmap(&rtp);
  auto sourceTime = GST_BUFFER_PTS(buffer);
  if (auto *event = gst_pad_get_sticky_event(pad, GST_EVENT_SEGMENT, 0)) {
    const GstSegment *segment = nullptr;
    gst_event_parse_segment(event, &segment);
    if (segment && segment->format == GST_FORMAT_TIME)
      sourceTime = gst_segment_to_stream_time(segment, GST_FORMAT_TIME, sourceTime);
    gst_event_unref(event);
  }
  std::lock_guard lock(s->mutex);
  for (auto &input : s->inputs)
    if (input.pts == sourceTime) {
      if (!input.emitted) {
        input.emitted = true;
        input.correlation.ssrc = key.first;
        input.correlation.rtpTimestamp = key.second;
        if (s->outboundMaps.size() >= 64) {
          s->fail("FrameCorrelationQueueFull");
          return GST_PAD_PROBE_DROP;
        }
        s->outboundMaps.push_back(input.correlation);
      }
      return GST_PAD_PROBE_OK;
    }
  s->pipelineDiagnostic = "rtpPts=" + QString::number(GST_BUFFER_PTS(buffer)) +
                          " streamTime=" + QString::number(sourceTime) +
                          " lastInput=" + QString::number(s->inputs.empty() ? 0 : s->inputs.back().pts);
  s->fail("RtpSourceCorrelationMissing");
  return GST_PAD_PROBE_DROP;
}
template <class Inspect> GstPadProbeReturn inspectRtp(const Shared &s, GstPadProbeInfo *info, Inspect inspect)
{
  if (s->closing.load() || s->failure.load() || !s->pairApproved.load())
    return GST_PAD_PROBE_DROP;
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
    auto *list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
    if (!gst_buffer_list_length(list))
      return GST_PAD_PROBE_DROP;
    for (guint i = 0; i < gst_buffer_list_length(list); ++i)
      if (inspect(gst_buffer_list_get(list, i)) != GST_PAD_PROBE_OK)
        return GST_PAD_PROBE_DROP;
    return GST_PAD_PROBE_OK;
  }
  return inspect(GST_PAD_PROBE_INFO_BUFFER(info));
}
GstPadProbeReturn outgoingRtp(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  auto s = state(data);
  if (!s->profile.load()) {
    ++s->deniedPackets;
    return GST_PAD_PROBE_DROP;
  }
  return inspectRtp(s, info, [&](GstBuffer *buffer) { return inspectOutgoing(s, pad, buffer); });
}
GstPadProbeReturn inspectIncoming(const Shared &s, GstBuffer *buffer)
{
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  if (!gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp))
    return GST_PAD_PROBE_DROP;
  RtpKey key{gst_rtp_buffer_get_ssrc(&rtp), gst_rtp_buffer_get_timestamp(&rtp)};
  gst_rtp_buffer_unmap(&rtp);
  std::lock_guard lock(s->mutex);
  s->decodeTimes[GST_BUFFER_PTS(buffer)] = key;
  while (s->decodeTimes.size() > 64)
    s->decodeTimes.erase(s->decodeTimes.begin());
  return GST_PAD_PROBE_OK;
}
GstPadProbeReturn incomingRtp(GstPad *, GstPadProbeInfo *info, gpointer data)
{
  auto s = state(data);
  return inspectRtp(s, info, [&](GstBuffer *buffer) { return inspectIncoming(s, buffer); });
}
void deepAdded(GstBin *, GstBin *, GstElement *element, gpointer data)
{
  auto s = state(data);
  auto *factory = gst_element_get_factory(element);
  if (!factory)
    return;
  const char *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
  if (!g_strcmp0(name, "rtpgccbwe"))
    connect(s, G_OBJECT(element), "notify::estimated-bitrate", G_CALLBACK(rateChanged));
}
void consumerCreated(GstElement *, const gchar *, GstPipeline *pipeline, gpointer data)
{
  auto s = state(data);
  connect(s, G_OBJECT(pipeline), "deep-element-added", G_CALLBACK(deepAdded));
}
gboolean encoderSetup(GstElement *, const gchar *peer, const gchar *, GstElement *encoder, gpointer data)
{
  auto s = state(data);
  auto *factory = gst_element_get_factory(encoder);
  if (!factory || g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "x264enc")) {
    s->fail("SoftwareEncoderRequired");
    return FALSE;
  }
  gst_util_set_object_arg(G_OBJECT(encoder), "tune", "zerolatency");
  gst_util_set_object_arg(G_OBJECT(encoder), "speed-preset", "ultrafast");
  g_object_set(encoder, "key-int-max", 30u, "bframes", 0u, "rc-lookahead", 0, "threads", 2u, nullptr);
  if (peer && QString::fromUtf8(peer) == s->config.binding.peerIdentity) {
    GstElement *previous = nullptr;
    {
      std::lock_guard lock(s->mutex);
      previous = s->encoder;
      s->encoder = GST_ELEMENT(gst_object_ref(encoder));
    }
    if (previous)
      gst_object_unref(previous);
    connect(s, G_OBJECT(encoder), "notify::bitrate", G_CALLBACK(encoderRate));
    probe(
        s, gst_element_get_static_pad(encoder, "src"),
        GstPadProbeType(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_BUFFER), encoderProbe
    );
  }
  return TRUE;
}
gboolean payloaderSetup(GstElement *, const gchar *peer, const gchar *, GstElement *payloader, gpointer data)
{
  auto s = state(data);
  if (peer && QString::fromUtf8(peer) == s->config.binding.peerIdentity)
    probe(
        s, gst_element_get_static_pad(payloader, "src"),
        GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST), outgoingRtp
    );
  return FALSE; // Keep SDK public default RTP payload configuration.
}
void publishPending(const Shared &s)
{
  std::lock_guard lock(s->mutex);
  for (auto it = s->pending.begin(); it != s->pending.end();) {
    const auto mapping = s->inboundMaps.find(it->key);
    if (mapping == s->inboundMaps.end()) {
      ++it;
      continue;
    }
    const auto &m = mapping->second;
    FrameDescriptor d;
    d.session = s->config.binding.token.session;
    d.sourceResource = s->config.streamSourceResource.toStdString();
    d.topologyGeneration = m.topologyGeneration;
    d.epoch = m.epoch;
    d.sequence = m.frame;
    d.captureSequence = m.captureFrame;
    d.captureClockId = "remote." + m.captureClockId.toStdString();
    d.captureNanoseconds = m.captureNanoseconds;
    d.captureTimeValid = true;
    d.repeated = m.repeated;
    d.width = d.logicalWidth = 1920;
    d.height = d.logicalHeight = 1080;
    d.visible = {0, 0, 1920, 1080};
    d.planes = {{0, 7680, 8294400}};
    d.color = ColorProfile::SdrBt709Full8;
    if (m.frame <= s->lastDecoded)
      s->output->abortWrite(it->ticket);
    else if (!s->output->publish(it->ticket, std::move(d))) {
      s->output->abortWrite(it->ticket);
      s->fail("DecodedFrameMetadataRejected");
    } else {
      s->lastDecoded = m.frame;
      ++s->decodedFrames;
    }
    it = s->pending.erase(it);
  }
}
GstFlowReturn decodedSample(GstAppSink *sink, gpointer data)
{
  auto s = state(data);
  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample)
    return GST_FLOW_EOS;
  if (s->closing.load() || !s->pairApproved.load() || !s->profile.load()) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }
  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstVideoInfo video;
  gst_video_info_init(&video);
  if (!gst_video_info_from_caps(&video, gst_sample_get_caps(sample)) ||
      GST_VIDEO_INFO_FORMAT(&video) != GST_VIDEO_FORMAT_BGRA || GST_VIDEO_INFO_WIDTH(&video) != 1920 ||
      GST_VIDEO_INFO_HEIGHT(&video) != 1080 || video.colorimetry.range != GST_VIDEO_COLOR_RANGE_0_255 ||
      video.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_RGB ||
      video.colorimetry.transfer != GST_VIDEO_TRANSFER_BT709 ||
      video.colorimetry.primaries != GST_VIDEO_COLOR_PRIMARIES_BT709) {
    s->fail("DecodedFormatMismatch");
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }
  RtpKey key{};
  bool correlated = false;
  {
    std::lock_guard lock(s->mutex);
    const auto found = s->decodeTimes.find(GST_BUFFER_PTS(buffer));
    if (found != s->decodeTimes.end()) {
      key = found->second;
      correlated = true;
    }
    if (s->pending.size() >= 2) {
      s->output->abortWrite(s->pending.front().ticket);
      s->pending.pop_front();
    }
  }
  if (!correlated) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }
  auto ticket = s->output->reserve(8294400, MemoryDomain::Cpu);
  if (!ticket) {
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }
  GstVideoFrame frame;
  if (!gst_video_frame_map(&frame, &video, buffer, GST_MAP_READ)) {
    s->output->abortWrite(*ticket);
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }
  auto owner = std::shared_ptr<unsigned char[]>(new (std::nothrow) unsigned char[8294400]);
  if (!owner) {
    gst_video_frame_unmap(&frame);
    s->output->abortWrite(*ticket);
    gst_sample_unref(sample);
    s->fail("DecodedAllocationFailed");
    return GST_FLOW_ERROR;
  }
  const auto *pixels = static_cast<const unsigned char *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
  const auto stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
  if (stride < 7680) {
    s->fail("DecodedStrideInvalid");
    gst_video_frame_unmap(&frame);
    s->output->abortWrite(*ticket);
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }
  for (size_t row = 0; row < 1080; ++row)
    std::memcpy(owner.get() + row * 7680, pixels + row * stride, 7680);
  gst_video_frame_unmap(&frame);
  gst_sample_unref(sample);
  const void *view = owner.get();
  std::shared_ptr<void> allocation(owner, owner.get());
  owner.reset();
  if (!s->output->adopt(*ticket, {std::move(allocation), 8294400, MemoryDomain::Cpu, view})) {
    s->output->abortWrite(*ticket);
    return GST_FLOW_FLUSHING;
  }
  {
    std::lock_guard lock(s->mutex);
    if (s->closing.load()) {
      s->output->abortWrite(*ticket);
      return GST_FLOW_FLUSHING;
    }
    s->pending.push_back({*ticket, key});
  }
  publishPending(s);
  return GST_FLOW_OK;
}
GstCaps *rawCaps(const char *format)
{
  GstVideoColorimetry color{
      GST_VIDEO_COLOR_RANGE_0_255, GST_VIDEO_COLOR_MATRIX_RGB, GST_VIDEO_TRANSFER_BT709, GST_VIDEO_COLOR_PRIMARIES_BT709
  };
  gchar *text = gst_video_colorimetry_to_string(&color);
  auto *caps = gst_caps_new_simple(
      "video/x-raw", "format", G_TYPE_STRING, format, "width", G_TYPE_INT, 1920, "height", G_TYPE_INT, 1080,
      "framerate", GST_TYPE_FRACTION, 30, 1, "colorimetry", G_TYPE_STRING, text, nullptr
  );
  g_free(text);
  return caps;
}
} // namespace
struct NativeWebRtcEngine::Impl
{
  Shared state = std::make_shared<State>();
};
bool NativeWebRtcEngine::initializeRuntime(const QString &root, QString &error)
{
  std::lock_guard lock(runtimeMutex);
  error.clear();
  if (poisoned.load()) {
    error = "NativeRuntimeQuarantined";
    return false;
  }
  const auto canonical = QFileInfo(root).canonicalFilePath();
  if (canonical.isEmpty() || canonical != root || !QFileInfo(root).isAbsolute()) {
    error = "NativeRuntimePath";
    return false;
  }
  if (runtimeReady) {
    if (runtimeRoot == root)
      return true;
    error = "NativeRuntimeAlreadyBound";
    return false;
  }
  for (const char *symbol : {"gst_version", "nice_agent_new", "gst_codec_utils_h264_get_profile_flags_level"}) {
    Dl_info image{};
    const auto address = dlsym(RTLD_DEFAULT, symbol);
    if (!address || !dladdr(address, &image) || !image.dli_fname ||
        !QFileInfo(QString::fromLocal8Bit(image.dli_fname)).canonicalFilePath().startsWith(root + "/lib/")) {
      error = "NativeCoreRuntimeMismatch";
      return false;
    }
  }
  guint major = 0, minor = 0, micro = 0, nano = 0;
  gst_version(&major, &minor, &micro, &nano);
  if (major != GST_VERSION_MAJOR || minor != GST_VERSION_MINOR || micro != GST_VERSION_MICRO ||
      nano != GST_VERSION_NANO) {
    error = "NativeRuntimeVersionMismatch";
    return false;
  }
  if (gst_is_initialized()) {
    error = "NativeRuntimeAlreadyInitialized";
    return false;
  }
  for (const auto &entry : gstreamerBuildPlugins) {
    WorkerRegistration plugin{
        root + "/lib/gstreamer-1.0/" + entry.file, entry.sha256, "gstreamer.runtime", "plugin.manifest"
    };
    if (!validWorkerExecutable(plugin)) {
      error = "NativePluginManifestRejected";
      return false;
    }
  }
  registryDirectory = std::make_unique<QTemporaryDir>();
  if (!registryDirectory->isValid()) {
    error = "NativeRegistryDirectory";
    return false;
  }
  qputenv("GST_PLUGIN_PATH", "");
  qputenv("GST_PLUGIN_PATH_1_0", "");
  qputenv("GST_PLUGIN_SYSTEM_PATH", "");
  qputenv("GST_PLUGIN_SYSTEM_PATH_1_0", "");
  qputenv("GST_REGISTRY", QFile::encodeName(registryDirectory->path() + "/registry.bin"));
  qputenv("GST_REGISTRY_FORK", "no");
  GError *failure = nullptr;
  if (!gst_init_check(nullptr, nullptr, &failure)) {
    if (failure)
      g_error_free(failure);
    error = "NativeRuntimeInitFailed";
    poisoned.store(true);
    return false;
  }
  for (const auto &entry : gstreamerBuildPlugins) {
    const auto path = QFile::encodeName(root + "/lib/gstreamer-1.0/" + entry.file);
    auto *plugin = gst_plugin_load_file(path.constData(), &failure);
    if (!plugin) {
      if (failure)
        g_error_free(failure);
      error = "NativePluginLoadFailed";
      poisoned.store(true);
      return false;
    }
    gst_object_unref(plugin);
  }
  for (const auto *name : {"vtenc_h264", "vtenc_h264_hw"})
    if (auto *factory = gst_element_factory_find(name)) {
      gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_NONE);
      gst_object_unref(factory);
    }
  for (const auto *name :
       {"appsrc",     "appsink",   "errorignore", "videoconvert", "videorate",  "x264enc",      "avdec_h264",
        "webrtcsink", "webrtcsrc", "webrtcbin",   "rtpgccbwe",    "rtph264pay", "rtph264depay", "h264parse",
        "nicesrc",    "nicesink",  "dtlsenc",     "dtlsdec",      "srtpenc",    "srtpdec"}) {
    auto *factory = gst_element_factory_find(name);
    if (!factory) {
      error = "NativeRequiredFactoryMissing";
      poisoned.store(true);
      return false;
    }
    if (!g_strcmp0(name, "x264enc"))
      gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_PRIMARY + 100);
    gst_object_unref(factory);
  }
  runtimeRoot = root;
  runtimeReady = true;
  return true;
}
NativeWebRtcEngine::NativeWebRtcEngine() : m_impl(std::make_unique<Impl>())
{
}
NativeWebRtcEngine::~NativeWebRtcEngine()
{
  stop();
}
bool NativeWebRtcEngine::prepare(const NativeWebRtcConfiguration &config)
{
  auto s = m_impl->state;
  if (!runtimeReady || poisoned.load() || s->prepared || s->closing.load()) {
    s->fail("NativePrepareState");
    return false;
  }
  const auto &p = config.preparation;
  const auto &b = config.binding;
  WorkerProtocol identity(WorkerSide::Worker, b, QByteArray(32, 'x'), Clock::now());
  const QHostAddress local(config.localAddress);
  const auto localAddresses = QNetworkInterface::allAddresses();
  if (identity.failed() || b.purpose != WorkerPurpose::Media || b.backend != "webrtc" ||
      !validWorkerPrepare(p, b.direction, Clock::now()) || p.protocol != WireProtocol::WebRtc ||
      p.video.width != 1920 || p.video.height != 1080 || p.video.fpsNumerator != 30 || p.video.fpsDenominator != 1 ||
      p.video.graphics != GraphicsMode::H264ConstrainedBaseline || p.video.color != ColorProfile::SdrBt709Limited8 ||
      p.video.cursor != CursorMode::Embedded || p.encodedQueueFrames != 2 || p.encodedQueueBytes != 4 * 1024 * 1024 ||
      !validId(config.streamSourceResource.toStdString(), 16) || !config.streamTopologyGeneration || local.isNull() ||
      !localAddresses.contains(local) || (local.isLoopback() && !p.allowLoopbackDiagnostics) ||
      (b.direction == Direction::Send && (config.streamSourceResource != p.sourceResource ||
                                          config.streamTopologyGeneration != p.sourceTopologyGeneration))) {
    s->fail("NativeConfigurationRejected");
    return false;
  }
  s->config = config;
  s->budget = std::make_shared<FrameBudget>(
      std::min(p.rawFrameSlots, 3u), std::min<uint64_t>(p.rawFrameBytes, 32 * 1024 * 1024)
  );
  s->parser = nice_agent_new(g_main_context_default(), NICE_COMPATIBILITY_RFC5245);
  g_object_set(s->parser, "upnp", FALSE, "ice-tcp", FALSE, nullptr);
  s->parserStream = nice_agent_add_stream(s->parser, 1);
  if (!s->parserStream) {
    s->fail("CandidateParserUnavailable");
    return false;
  }
  GError *failure = nullptr;
  const bool sender = b.direction == Direction::Send;
  const char *graph =
      sender ? "appsrc name=input ! videoconvert ! video/x-raw,format=I420,colorimetry=bt709 ! webrtcsink name=endpoint"
             : "webrtcsrc name=endpoint ! rtph264depay name=depay ! h264parse name=parser ! "
               "video/x-h264,stream-format=avc,alignment=au ! avdec_h264 ! videoconvert ! "
               "appsink name=output";
  s->pipeline = gst_parse_launch(graph, &failure);
  if (!s->pipeline || failure) {
    if (failure)
      g_error_free(failure);
    s->fail("NativePipelineConstruction");
    return false;
  }
  s->signallingElement = gst_bin_get_by_name(GST_BIN(s->pipeline), "endpoint");
  g_object_get(s->signallingElement, "signaller", &s->signaller, nullptr);
  if (!s->signaller) {
    s->fail("NativeSignallerUnavailable");
    return false;
  }
  connect(s, s->signaller, "start", G_CALLBACK(startHandled));
  connect(s, s->signaller, "stop", G_CALLBACK(handled));
  connect(s, s->signaller, "end-session", G_CALLBACK(endHandled));
  connect(s, s->signaller, "webrtcbin-ready", G_CALLBACK(webrtcReady));
  connect(s, s->signaller, "send-session-description", G_CALLBACK(sendSdp));
  connect(s, s->signaller, "send-ice", G_CALLBACK(sendIce));
  g_object_set(
      s->signallingElement, "stun-server", nullptr, "enable-control-data-channel", FALSE,
      "enable-data-channel-navigation", FALSE, nullptr
  );
  for (const char *property : {"run-web-server", "run-signalling-server"})
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(s->signallingElement), property))
      g_object_set(s->signallingElement, property, FALSE, nullptr);
  if (sender) {
    GstCaps *video = gst_caps_from_string("video/x-h264,profile=constrained-baseline,stream-format=avc,alignment=au"),
            *audio = gst_caps_new_empty();
    g_object_set(
        s->signallingElement, "video-caps", video, "audio-caps", audio, "congestion-control", 2, "min-bitrate", 500000u,
        "max-bitrate", 8000000u, "start-bitrate", 2000000u, nullptr
    );
    gst_caps_unref(video);
    gst_caps_unref(audio);
    connect(s, G_OBJECT(s->signallingElement), "consumer-pipeline-created", G_CALLBACK(consumerCreated));
    connect(s, G_OBJECT(s->signallingElement), "encoder-setup", G_CALLBACK(encoderSetup));
    if (!g_signal_lookup("payloader-setup", G_OBJECT_TYPE(s->signallingElement))) {
      s->fail("NativeRtpHookUnsupported");
      return false;
    }
    connect(s, G_OBJECT(s->signallingElement), "payloader-setup", G_CALLBACK(payloaderSetup));
    s->source = gst_bin_get_by_name(GST_BIN(s->pipeline), "input");
    auto *caps = rawCaps("BGRA");
    gst_app_src_set_caps(GST_APP_SRC(s->source), caps);
    gst_caps_unref(caps);
    g_object_set(
        s->source, "is-live", TRUE, "format", GST_FORMAT_TIME, "block", FALSE, "do-timestamp", FALSE, "max-buffers",
        guint64(2), "max-bytes", guint64(2 * 8294400), "leaky-type", GST_APP_LEAKY_TYPE_DOWNSTREAM, nullptr
    );
  } else {
    gst_util_set_object_arg(G_OBJECT(s->signallingElement), "video-codecs", "<H264>");
    gst_util_set_object_arg(G_OBJECT(s->signallingElement), "audio-codecs", "<>");
    auto *output = gst_bin_get_by_name(GST_BIN(s->pipeline), "output");
    auto *caps = rawCaps("BGRA");
    gst_app_sink_set_caps(GST_APP_SINK(output), caps);
    gst_caps_unref(caps);
    g_object_set(output, "max-buffers", guint(1), "drop", TRUE, "sync", FALSE, "emit-signals", TRUE, nullptr);
    connect(s, G_OBJECT(output), "new-sample", G_CALLBACK(decodedSample));
    gst_object_unref(output);
    auto *parser = gst_bin_get_by_name(GST_BIN(s->pipeline), "parser");
    probe(
        s, gst_element_get_static_pad(parser, "src"),
        GstPadProbeType(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_BUFFER), encoderProbe
    );
    gst_object_unref(parser);
    auto *depay = gst_bin_get_by_name(GST_BIN(s->pipeline), "depay");
    probe(
        s, gst_element_get_static_pad(depay, "sink"),
        GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST), incomingRtp
    );
    gst_object_unref(depay);
    s->output = std::make_shared<FramePool>(b.token.worker, uint32_t(b.token.epoch), b.token.session, s->budget);
  }
  if (gst_element_set_state(s->pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
    s->fail("NativePipelinePrepareFailed");
    return false;
  }
  s->prepared = true;
  s->nativeStopped.store(false);
  return true;
}
bool NativeWebRtcEngine::start()
{
  auto s = m_impl->state;
  if (!s->prepared || s->started || s->closing.load() || s->failure.load())
    return false;
  s->started = true;
  if (gst_element_set_state(s->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    s->fail("NativePipelineStartFailed");
    return false;
  }
  return !s->failure.load();
}
bool NativeWebRtcEngine::applySignal(const WorkerMediaSignal &signal)
{
  auto s = m_impl->state;
  if (!s->started || s->closing.load() || s->failure.load() || !validMediaSignal(signal))
    return false;
  if (!s->sessionStarted) {
    const auto bytes =
        signal.description.toUtf8().size() + signal.candidate.toUtf8().size() + signal.mid.toUtf8().size();
    if (s->startupSignals.size() >= 72 || bytes > 256 * 1024 - s->startupSignalBytes) {
      s->fail("MediaStartupSignalQueueFull");
      return false;
    }
    s->startupSignals.push_back(signal);
    s->startupSignalBytes += bytes;
    return true;
  }
  const auto session = s->config.binding.planHash.toLatin1();
  if (signal.kind == WebRtcSignalKind::EndOfCandidates)
    return true;
  if (signal.kind == WebRtcSignalKind::Ice) {
    if (!candidate(s, signal.candidate)) {
      s->fail("IceCandidateDenied");
      return false;
    }
    const auto ice = signal.candidate.toUtf8(), mid = signal.mid.toUtf8();
    g_signal_emit_by_name(
        s->signaller, "handle-ice", session.constData(), signal.mediaLine, mid.constData(), ice.constData()
    );
    return true;
  }
  const bool offer = signal.kind == WebRtcSignalKind::Offer;
  if (offer != (s->config.binding.direction == Direction::Receive)) {
    s->fail("SdpRoleMismatch");
    return false;
  }
  const auto text = signal.description.toUtf8();
  const auto digest = QString::fromLatin1(QCryptographicHash::hash(text, QCryptographicHash::Sha256).toHex());
  if (!s->remoteSdpDigest.isEmpty()) {
    if (s->remoteSdpDigest == digest)
      return true;
    s->fail("SdpRenegotiationDenied");
    return false;
  }
  GstSDPMessage *message = nullptr;
  gst_sdp_message_new(&message);
  bool safe = message &&
              gst_sdp_message_parse_buffer(reinterpret_cast<const guint8 *>(text.constData()), text.size(), message) ==
                  GST_SDP_OK &&
              gst_sdp_message_medias_len(message) == 1;
  if (safe) {
    const auto *media = gst_sdp_message_get_media(message, 0);
    const char *fingerprint = gst_sdp_media_get_attribute_val(media, "fingerprint");
    if (!fingerprint)
      fingerprint = gst_sdp_message_get_attribute_val(message, "fingerprint");
    const QRegularExpression fingerprintShape("^sha-256 (?:[0-9A-Fa-f]{2}:){31}[0-9A-Fa-f]{2}$");
    safe = !g_strcmp0(gst_sdp_media_get_media(media), "video") &&
           !g_strcmp0(gst_sdp_media_get_proto(media), "UDP/TLS/RTP/SAVPF") && gst_sdp_media_get_port(media) != 0 &&
           gst_sdp_media_get_attribute_val(media, "rtcp-mux") && fingerprint &&
           fingerprintShape.match(QString::fromLatin1(fingerprint)).hasMatch() &&
           gst_sdp_media_get_attribute_val(media, offer ? "sendonly" : "recvonly");
    for (guint i = 0; safe && i < gst_sdp_media_attributes_len(media); ++i) {
      const auto *attribute = gst_sdp_media_get_attribute(media, i);
      if (!g_strcmp0(attribute->key, "candidate"))
        safe = candidate(s, "candidate:" + QString::fromUtf8(attribute->value));
    }
  }
  if (!safe) {
    if (message)
      gst_sdp_message_free(message);
    s->fail("SdpPolicyDenied");
    return false;
  }
  s->remoteSdpDigest = digest;
  auto *description =
      gst_webrtc_session_description_new(offer ? GST_WEBRTC_SDP_TYPE_OFFER : GST_WEBRTC_SDP_TYPE_ANSWER, message);
  g_signal_emit_by_name(s->signaller, "session-description", session.constData(), description);
  gst_webrtc_session_description_free(description);
  return true;
}
namespace {
bool pushInput(const Shared &s, std::shared_ptr<CpuFrameLease> lease, bool repeated)
{
  if (!lease || s->closing.load() || s->failure.load())
    return false;
  const auto &d = lease->descriptor();
  if (d.session != s->config.binding.token.session ||
      d.sourceResource != s->config.streamSourceResource.toStdString() ||
      d.topologyGeneration != s->config.streamTopologyGeneration || d.epoch != s->config.binding.token.epoch ||
      d.width != 1920 || d.height != 1080 || d.pixelFormat != PixelFormat::Bgra8 ||
      d.color != ColorProfile::SdrBt709Full8 || d.rotation != 0 || d.logicalWidth != 1920 || d.logicalHeight != 1080 ||
      d.cursor != CursorMode::Embedded || d.visible.x != 0 || d.visible.y != 0 || d.visible.width != 1920 ||
      d.visible.height != 1080 || (!repeated && d.repeated) || !d.captureTimeValid || d.captureClockId.empty() ||
      d.captureClockId.size() > 56 || d.planes.size() != 1 || d.planes[0].offset != 0 || d.planes[0].stride != 7680 ||
      d.planes[0].length != 8294400 || lease->allocationBytes() < 8294400 ||
      (!repeated && (d.sequence <= s->inputSequence || d.captureNanoseconds <= s->inputCapture))) {
    s->fail("NativeInputBindingRejected");
    return false;
  }
  if (s->mediaSequence == UINT64_MAX) {
    s->fail("MediaFrameSequenceExhausted");
    return false;
  }
  auto *clock = gst_element_get_clock(s->pipeline);
  if (!clock)
    return false;
  auto pts = gst_clock_get_time(clock) - gst_element_get_base_time(s->pipeline);
  gst_object_unref(clock);
  if (s->lastPts != GST_CLOCK_TIME_NONE && pts <= s->lastPts)
    pts = s->lastPts + 1;
  s->lastPts = pts;
  NativeFrameCorrelation map;
  map.sourceResource = QString::fromStdString(d.sourceResource);
  map.captureClockId = QString::fromStdString(d.captureClockId);
  map.topologyGeneration = d.topologyGeneration;
  map.frame = ++s->mediaSequence;
  map.captureFrame = d.sequence;
  map.captureNanoseconds = d.captureNanoseconds;
  map.epoch = d.epoch;
  map.repeated = repeated;
  {
    std::lock_guard lock(s->mutex);
    s->inputs.push_back({pts, map});
    while (s->inputs.size() > 64)
      s->inputs.pop_front();
    s->lastInput = lease;
  }
  auto *owner = new std::shared_ptr<CpuFrameLease>(std::move(lease));
  auto *buffer = gst_buffer_new_wrapped_full(
      GST_MEMORY_FLAG_READONLY, const_cast<void *>((*owner)->data()), 8294400, 0, 8294400, owner,
      [](gpointer data) { delete static_cast<std::shared_ptr<CpuFrameLease> *>(data); }
  );
  if (!buffer) {
    delete owner;
    s->fail("NativeInputBufferAllocation");
    return false;
  }
  GST_BUFFER_PTS(buffer) = pts;
  GST_BUFFER_DTS(buffer) = pts;
  if (gst_app_src_push_buffer(GST_APP_SRC(s->source), buffer) != GST_FLOW_OK) {
    s->fail("NativeInputFlowFailed");
    return false;
  }
  if (!repeated) {
    s->inputSequence = d.sequence;
    s->inputCapture = d.captureNanoseconds;
    ++s->inputFrames;
  }
  return true;
}
} // namespace
bool NativeWebRtcEngine::submit(std::shared_ptr<CpuFrameLease> frame)
{
  auto s = m_impl->state;
  return s->started && s->config.binding.direction == Direction::Send && pushInput(s, std::move(frame), false);
}
std::shared_ptr<CpuFrameLease> NativeWebRtcEngine::takeDecoded()
{
  auto s = m_impl->state;
  if (s->closing.load() || s->failure.load() || !s->pairApproved.load() || !s->output)
    return {};
  return CpuFrameLease::acquire(s->output);
}
std::vector<WorkerMediaSignal> NativeWebRtcEngine::takeSignals()
{
  auto s = m_impl->state;
  std::lock_guard lock(s->mutex);
  std::vector<WorkerMediaSignal> values(s->outboundSignals.begin(), s->outboundSignals.end());
  s->outboundSignals.clear();
  s->signalBytes = 0;
  return values;
}
std::vector<NativeFrameCorrelation> NativeWebRtcEngine::takeCorrelations()
{
  auto s = m_impl->state;
  std::lock_guard lock(s->mutex);
  std::vector<NativeFrameCorrelation> values(s->outboundMaps.begin(), s->outboundMaps.end());
  s->outboundMaps.clear();
  return values;
}
bool NativeWebRtcEngine::applyCorrelation(const NativeFrameCorrelation &m)
{
  auto s = m_impl->state;
  if (!s->started || s->closing.load() || s->failure.load() || s->config.binding.direction != Direction::Receive ||
      m.sourceResource != s->config.streamSourceResource ||
      m.topologyGeneration != s->config.streamTopologyGeneration || m.epoch != s->config.binding.token.epoch ||
      !validFrameCorrelation(m))
    return false;
  {
    std::lock_guard lock(s->mutex);
    RtpKey key{m.ssrc, m.rtpTimestamp};
    auto found = s->inboundMaps.find(key);
    if (found != s->inboundMaps.end()) {
      if (found->second == m)
        return true;
      s->fail("FrameCorrelationConflict");
      return false;
    }
    s->inboundMaps[key] = m;
    s->mapOrder.push_back(key);
    while (s->mapOrder.size() > 64) {
      s->inboundMaps.erase(s->mapOrder.front());
      s->mapOrder.pop_front();
    }
  }
  publishPending(s);
  return true;
}
void NativeWebRtcEngine::poll()
{
  auto s = m_impl->state;
  if (!s->started || s->closing.load())
    return;
  for (int i = 0; i < 16 && g_main_context_pending(nullptr); ++i)
    g_main_context_iteration(nullptr, FALSE);
  if (!s->sessionStarted && s->signallerReady.load()) {
    const auto session = s->config.binding.planHash.toLatin1(), peer = s->config.binding.peerIdentity.toLatin1();
    if (s->config.binding.direction == Direction::Send)
      g_signal_emit_by_name(s->signaller, "session-requested", session.constData(), peer.constData(), nullptr);
    else
      g_signal_emit_by_name(s->signaller, "session-started", session.constData(), peer.constData());
    s->sessionStarted = true;
  }
  if (s->sessionStarted) {
    auto queued = std::move(s->startupSignals);
    s->startupSignals.clear();
    s->startupSignalBytes = 0;
    for (const auto &signal : queued)
      if (!applySignal(signal))
        break;
  }
  auto *bus = gst_element_get_bus(s->pipeline);
  while (auto *message = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError *error = nullptr;
      gchar *debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      if (error) {
        std::lock_guard lock(s->mutex);
        const char *factoryName = "core";
        if (GST_IS_ELEMENT(message->src))
          if (auto *factory = gst_element_get_factory(GST_ELEMENT(message->src)))
            factoryName = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
        s->pipelineDiagnostic = QString::fromLatin1(factoryName) + "/" +
                                QString::fromLatin1(g_quark_to_string(error->domain)) + "/" +
                                QString::number(error->code);
        g_error_free(error);
      }
      g_free(debug);
    }
    s->fail("NativePipelineError");
    gst_message_unref(message);
  }
  gst_object_unref(bus);
  if (s->refreshPending.exchange(false) && s->config.binding.direction == Direction::Send) {
    refresh();
    std::shared_ptr<CpuFrameLease> last;
    {
      std::lock_guard lock(s->mutex);
      last = s->lastInput;
    }
    if (last)
      pushInput(s, std::move(last), true);
  }
}
bool NativeWebRtcEngine::refresh()
{
  auto s = m_impl->state;
  GstElement *encoder = nullptr;
  {
    std::lock_guard lock(s->mutex);
    if (s->encoder)
      encoder = GST_ELEMENT(gst_object_ref(s->encoder));
  }
  if (!encoder)
    return false;
  auto *pad = gst_element_get_static_pad(encoder, "src");
  const bool sent =
      pad && gst_pad_send_event(pad, gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0));
  if (pad)
    gst_object_unref(pad);
  gst_object_unref(encoder);
  return sent;
}
bool NativeWebRtcEngine::stop()
{
  auto s = m_impl->state;
  if (s->closing.exchange(true))
    return stopped();
  s->pairApproved.store(false);
  bool native = true;
  if (s->pipeline) {
    native = gst_element_set_state(s->pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE;
    if (native)
      native = gst_element_get_state(s->pipeline, nullptr, nullptr, GST_SECOND) == GST_STATE_CHANGE_SUCCESS;
  }
  if (!native) {
    s->fail("NativeStopUnconfirmed");
    poisoned.store(true);
    return false;
  }
  std::vector<Connection> connections;
  std::vector<Probe> probes;
  {
    std::lock_guard lock(s->connectionsMutex);
    connections.swap(s->connections);
    probes.swap(s->probes);
  }
  for (auto &p : probes) {
    gst_pad_remove_probe(p.pad, p.id);
    gst_object_unref(p.pad);
  }
  for (auto &c : connections) {
    g_signal_handler_disconnect(c.object, c.id);
    g_object_unref(c.object);
  }
  std::shared_ptr<CpuFrameLease> lastInput;
  {
    std::lock_guard lock(s->mutex);
    if (s->output) {
      s->output->retire();
      for (const auto &p : s->pending)
        s->output->abortWrite(p.ticket);
    }
    s->pending.clear();
    lastInput = std::move(s->lastInput);
    s->inputs.clear();
    s->outboundSignals.clear();
    s->startupSignals.clear();
    s->outboundMaps.clear();
  }
  lastInput.reset();
  if (s->source) {
    gst_object_unref(s->source);
    s->source = nullptr;
  }
  if (s->signallingElement) {
    gst_object_unref(s->signallingElement);
    s->signallingElement = nullptr;
  }
  if (s->signaller) {
    g_object_unref(s->signaller);
    s->signaller = nullptr;
  }
  if (s->pipeline) {
    gst_object_unref(s->pipeline);
    s->pipeline = nullptr;
  }
  s->nativeStopped.store(true);
  return stopped();
}
bool NativeWebRtcEngine::stopped() const
{
  auto s = m_impl->state;
  return s->nativeStopped.load() && (!s->output || s->output->drained());
}
QString NativeWebRtcEngine::error() const
{
  const auto *why = m_impl->state->failure.load();
  if (!why)
    return {};
  const auto s = m_impl->state;
  std::lock_guard lock(s->mutex);
  return QString::fromLatin1(why) + (s->pipelineDiagnostic.isEmpty() ? QString{} : ": " + s->pipelineDiagnostic);
}
NativeWebRtcStatistics NativeWebRtcEngine::statistics() const
{
  auto s = m_impl->state;
  return {s->inputFrames.load(),   s->decodedFrames.load(),
          s->gccUpdates.load(),    s->encoderUpdates.load(),
          s->deniedPackets.load(), s->estimated.load(),
          s->encoderRate.load(),   s->profile.load(),
          s->pairApproved.load(),  s->compatibleProfileLabels.load()};
}
} // namespace deskflow::display::infra
