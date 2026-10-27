// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: MIT
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <stdio.h>
// Development-only loopback fixture: signals carry SDP/ICE in memory.
// This does not implement application authentication or certify a display path.
static gint verified_profile = 0;
static GstPadProbeReturn encoded_caps(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  (void)pad;
  (void)data;
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
  if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
    GstCaps *caps = NULL;
    gst_event_parse_caps(event, &caps);
    const GstStructure *st = gst_caps_get_structure(caps, 0);
    const char *profile = gst_structure_get_string(st, "profile");
    if (profile && !g_strcmp0(profile, "constrained-baseline"))
      g_atomic_int_set(&verified_profile, 1);
  }
  return GST_PAD_PROBE_OK;
}
static gint ready_flags = 0, bitrate_updates = 0, encoder_updates = 0, selected_encoders = 0;
static gint estimated_min = 2147483647, estimated_max = 0, encoded_min = 2147483647, encoded_max = 0;
static void minmax(gint *low, gint *high, gint value)
{
  gint old;
  do {
    old = g_atomic_int_get(low);
    if (value >= old)
      break;
  } while (!g_atomic_int_compare_and_exchange(low, old, value));
  do {
    old = g_atomic_int_get(high);
    if (value <= old)
      break;
  } while (!g_atomic_int_compare_and_exchange(high, old, value));
}
static void bandwidth(GObject *object, GParamSpec *spec, gpointer data)
{
  (void)spec;
  (void)data;
  guint value = 0;
  g_object_get(object, "estimated-bitrate", &value, NULL);
  minmax(&estimated_min, &estimated_max, (gint)value);
  g_atomic_int_inc(&bitrate_updates);
}
static void encoder_bitrate(GObject *object, GParamSpec *spec, gpointer data)
{
  (void)spec;
  (void)data;
  guint value = 0;
  g_object_get(object, "bitrate", &value, NULL);
  minmax(&encoded_min, &encoded_max, (gint)value);
  g_atomic_int_inc(&encoder_updates);
}
static void deep_added(GstBin *top, GstBin *bin, GstElement *child, gpointer data)
{
  (void)top;
  (void)bin;
  (void)data;
  GstElementFactory *factory = gst_element_get_factory(child);
  if (factory && !g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "rtpgccbwe"))
    g_signal_connect(child, "notify::estimated-bitrate", G_CALLBACK(bandwidth), NULL);
}
static void consumer_pipeline(GstElement *sink, const gchar *peer, GstPipeline *pipeline, gpointer data)
{
  (void)sink;
  (void)peer;
  (void)data;
  g_signal_connect(pipeline, "deep-element-added", G_CALLBACK(deep_added), NULL);
}
static gboolean
encoder_setup(GstElement *sink, const gchar *peer, const gchar *stream, GstElement *encoder, gpointer data)
{
  (void)sink;
  (void)stream;
  (void)data;
  if (peer && !g_strcmp0(peer, "consumer")) {
    GstPad *encoded = gst_element_get_static_pad(encoder, "src");
    gst_pad_add_probe(encoded, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, encoded_caps, NULL, NULL);
    gst_object_unref(encoded);
    GstElementFactory *factory = gst_element_get_factory(encoder);
    if (!factory || g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "x264enc"))
      return FALSE;
    g_atomic_int_inc(&selected_encoders);
    g_signal_connect(encoder, "notify::bitrate", G_CALLBACK(encoder_bitrate), NULL);
  }
  gst_util_set_object_arg(G_OBJECT(encoder), "tune", "zerolatency");
  gst_util_set_object_arg(G_OBJECT(encoder), "speed-preset", "ultrafast");
  g_object_set(encoder, "key-int-max", 30u, "bframes", 0u, "rc-lookahead", 0, NULL);
  return TRUE;
}
static gboolean start_handled(GObject *object, gpointer role)
{
  (void)object;
  g_atomic_int_or(&ready_flags, GPOINTER_TO_INT(role));
  return TRUE;
}
static void restrict_ice(GObject *signaller, const gchar *peer, GstElement *webrtc, gpointer data)
{
  (void)signaller;
  (void)peer;
  (void)data;
  GObject *ice = NULL, *agent = NULL;
  g_object_get(webrtc, "ice-agent", &ice, NULL);
  if (ice && g_object_class_find_property(G_OBJECT_GET_CLASS(ice), "agent")) {
    g_object_get(ice, "agent", &agent, NULL);
    if (agent && g_object_class_find_property(G_OBJECT_GET_CLASS(agent), "upnp"))
      g_object_set(agent, "upnp", FALSE, NULL);
  }
  if (agent)
    g_object_unref(agent);
  if (ice)
    g_object_unref(ice);
}
typedef struct
{
  GObject *target;
  GstWebRTCSessionDescription *desc;
  gchar *candidate;
  gchar *mid;
  guint mline;
} Delivery;
static gboolean deliver(gpointer data)
{
  Delivery *d = data;
  if (d->desc)
    g_signal_emit_by_name(d->target, "session-description", "loop-session", d->desc);
  else
    g_signal_emit_by_name(d->target, "handle-ice", "loop-session", d->mline, d->mid, d->candidate);
  if (d->desc)
    gst_webrtc_session_description_free(d->desc);
  g_object_unref(d->target);
  g_free(d->candidate);
  g_free(d->mid);
  g_free(d);
  return G_SOURCE_REMOVE;
}
static gboolean send_sdp(GObject *self, const gchar *session, GstWebRTCSessionDescription *desc, gpointer target)
{
  (void)self;
  (void)session;
  Delivery *d = g_new0(Delivery, 1);
  d->target = g_object_ref(target);
  d->desc = gst_webrtc_session_description_copy(desc);
  g_main_context_invoke(NULL, deliver, d);
  return TRUE;
}
static gboolean
send_ice(GObject *self, const gchar *session, const gchar *candidate, guint mline, const gchar *mid, gpointer target)
{
  (void)self;
  (void)session;
  if (!candidate || !*candidate)
    return TRUE;
  Delivery *d = g_new0(Delivery, 1);
  d->target = g_object_ref(target);
  d->candidate = g_strdup(candidate);
  d->mid = g_strdup(mid);
  d->mline = mline;
  g_main_context_invoke(NULL, deliver, d);
  return TRUE;
}
static gboolean handled(GObject *self, gpointer data)
{
  (void)self;
  (void)data;
  return TRUE;
}
static gboolean end_session(GObject *self, const gchar *session, gpointer data)
{
  (void)self;
  (void)session;
  (void)data;
  return TRUE;
}
static void signaller_hooks(GObject *self, GObject *other, gint role)
{
  g_signal_connect(self, "start", G_CALLBACK(start_handled), GINT_TO_POINTER(role));
  g_signal_connect(self, "stop", G_CALLBACK(handled), NULL);
  g_signal_connect(self, "end-session", G_CALLBACK(end_session), NULL);
  g_signal_connect(self, "webrtcbin-ready", G_CALLBACK(restrict_ice), NULL);
  g_signal_connect(self, "send-session-description", G_CALLBACK(send_sdp), other);
  g_signal_connect(self, "send-ice", G_CALLBACK(send_ice), other);
}
static gint frames = 0, errors = 0;
static GMainLoop *loop;
static void handoff(GstElement *sink, GstBuffer *b, GstPad *pad, gpointer data)
{
  (void)sink;
  (void)b;
  (void)data;
  if (g_atomic_int_get(&frames) == 0) {
    GstCaps *caps = gst_pad_get_current_caps(pad);
    gchar *text = gst_caps_to_string(caps);
    printf("decoded_output_caps=%s\n", text);
    g_free(text);
    gst_caps_unref(caps);
  }
  g_atomic_int_inc(&frames);
}
static gboolean message(GstBus *bus, GstMessage *msg, gpointer data)
{
  (void)bus;
  (void)data;
  if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
    GError *e = NULL;
    gchar *debug = NULL;
    gst_message_parse_error(msg, &e, &debug);
    fprintf(stderr, "media error: %s\n%s\n", e->message, debug ? debug : "");
    g_error_free(e);
    g_free(debug);
    errors++;
    g_main_loop_quit(loop);
  }
  return G_SOURCE_CONTINUE;
}
typedef struct
{
  GObject *tx;
  GObject *rx;
} Signallers;
static gboolean begin(gpointer data)
{
  if (g_atomic_int_get(&ready_flags) != 3)
    return G_SOURCE_CONTINUE;
  Signallers *s = data;
  g_signal_emit_by_name(s->rx, "session-started", "loop-session", "producer");
  g_signal_emit_by_name(s->tx, "session-requested", "loop-session", "consumer", NULL);
  return G_SOURCE_REMOVE;
}
static gboolean finish(gpointer data)
{
  (void)data;
  g_main_loop_quit(loop);
  return G_SOURCE_REMOVE;
}
int main(int argc, char **argv)
{
  gst_init(&argc, &argv);
  const char *excluded[] = {"vtenc_h264", "vtenc_h264_hw", NULL};
  for (int i = 0; excluded[i]; i++) {
    GstPluginFeature *f = gst_registry_find_feature(gst_registry_get(), excluded[i], GST_TYPE_ELEMENT_FACTORY);
    if (f) {
      gst_plugin_feature_set_rank(f, GST_RANK_NONE);
      gst_object_unref(f);
    }
  }
  GstPluginFeature *selected = gst_registry_find_feature(gst_registry_get(), "x264enc", GST_TYPE_ELEMENT_FACTORY);
  if (selected) {
    gst_plugin_feature_set_rank(selected, GST_RANK_PRIMARY + 100);
    gst_object_unref(selected);
  }
  const char *required[] = {"webrtcsink", "webrtcsrc", "webrtcbin",  "rtpgccbwe", "nicesrc",    "nicesink",
                            "dtlsenc",    "dtlsdec",   "srtpenc",    "srtpdec",   "rtph264pay", "rtph264depay",
                            "h264parse",  "x264enc",   "avdec_h264", NULL};
  for (int i = 0; required[i]; i++) {
    GstElementFactory *f = gst_element_factory_find(required[i]);
    if (!f) {
      fprintf(stderr, "missing %s\n", required[i]);
      return 2;
    }
    gst_object_unref(f);
  }
  GError *error = NULL;
  GstElement *tx = gst_parse_launch(
      "videotestsrc is-live=true pattern=snow ! "
      "video/x-raw,format=I420,width=1920,height=1080,framerate=30/1,colorimetry=bt709 ! webrtcsink name=tx",
      &error
  );
  if (!tx || error) {
    fprintf(stderr, "TX parse: %s\n", error ? error->message : "");
    return 3;
  }
  GstElement *rx = gst_parse_launch(
      "webrtcsrc name=rx ! queue max-size-buffers=2 leaky=downstream ! rtph264depay ! h264parse ! avdec_h264 ! "
      "videoconvert ! video/x-raw,format=BGRA ! fakesink name=output sync=false signal-handoffs=true",
      &error
  );
  if (!rx || error) {
    fprintf(stderr, "RX parse: %s\n", error ? error->message : "");
    return 4;
  }
  GstElement *send = gst_bin_get_by_name(GST_BIN(tx), "tx"), *receive = gst_bin_get_by_name(GST_BIN(rx), "rx"),
             *output = gst_bin_get_by_name(GST_BIN(rx), "output");
  Signallers s = {0};
  g_object_get(send, "signaller", &s.tx, NULL);
  g_object_get(receive, "signaller", &s.rx, NULL);
  signaller_hooks(s.tx, s.rx, 1);
  signaller_hooks(s.rx, s.tx, 2);
  GstCaps *video = gst_caps_from_string("video/x-h264,profile=constrained-baseline"), *audio = gst_caps_new_empty();
  g_signal_connect(send, "consumer-pipeline-created", G_CALLBACK(consumer_pipeline), NULL);
  g_signal_connect(send, "encoder-setup", G_CALLBACK(encoder_setup), NULL);
  g_object_set(
      send, "stun-server", NULL, "congestion-control", 2, "min-bitrate", 500000u, "max-bitrate", 8000000u,
      "start-bitrate", 2000000u, "video-caps", video, "audio-caps", audio, "enable-control-data-channel", FALSE,
      "enable-data-channel-navigation", FALSE, NULL
  );
  g_object_set(
      receive, "stun-server", NULL, "enable-control-data-channel", FALSE, "enable-data-channel-navigation", FALSE, NULL
  );
  gst_util_set_object_arg(G_OBJECT(receive), "video-codecs", "<H264>");
  gst_util_set_object_arg(G_OBJECT(receive), "audio-codecs", "<>");
  gst_caps_unref(video);
  gst_caps_unref(audio);
  g_signal_connect(output, "handoff", G_CALLBACK(handoff), NULL);
  loop = g_main_loop_new(NULL, FALSE);
  GstBus *tb = gst_element_get_bus(tx), *rb = gst_element_get_bus(rx);
  gst_bus_add_watch(tb, message, NULL);
  gst_bus_add_watch(rb, message, NULL);
  gst_element_set_state(rx, GST_STATE_PLAYING);
  gst_element_set_state(tx, GST_STATE_PLAYING);
  g_timeout_add(10, begin, &s);
  g_timeout_add_seconds(8, finish, NULL);
  g_main_loop_run(loop);
  printf("verified_h264_constrained_baseline=%d\n", g_atomic_int_get(&verified_profile));
  printf(
      "received_decoded_frames=%d errors=%d selected_x264=%d gcc_updates=%d gcc_min_bps=%d gcc_max_bps=%d "
      "encoder_updates=%d encoder_min_kbps=%d encoder_max_kbps=%d\n",
      g_atomic_int_get(&frames), errors, g_atomic_int_get(&selected_encoders), g_atomic_int_get(&bitrate_updates),
      g_atomic_int_get(&estimated_min), g_atomic_int_get(&estimated_max), g_atomic_int_get(&encoder_updates),
      g_atomic_int_get(&encoded_min), g_atomic_int_get(&encoded_max)
  );
  gst_element_set_state(tx, GST_STATE_NULL);
  gst_element_set_state(rx, GST_STATE_NULL);
  gst_object_unref(tb);
  gst_object_unref(rb);
  gst_object_unref(output);
  gst_object_unref(send);
  gst_object_unref(receive);
  gst_object_unref(tx);
  gst_object_unref(rx);
  g_object_unref(s.tx);
  g_object_unref(s.rx);
  while (g_main_context_pending(NULL))
    g_main_context_iteration(NULL, FALSE);
  g_main_loop_unref(loop);
  return errors || frames < 30 || !verified_profile || selected_encoders < 1 || bitrate_updates < 1 ||
                 encoder_updates < 2
             ? 5
             : 0;
}
