#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Experimental authenticated live display session; native product UI is separate."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import secrets
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from display_transport import (ACCEPT, BIND, CAPS, CONFIG, HELLO, PING, PONG, PRESENT, READY,
                               RELEASE, START, STATS, STOP, STOPPED, VIDEO, ZERO,
                               BindingTokens, FramedSocket, TransferMetrics, TransportError,
                               authenticate, decode_json, fingerprint, init_identity,
                               require_fields, tls_context)
from h264_access_units import AccessUnitSplitter, MAX_AU, nal_types


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise TransportError("Integer out of range")
    return value


def hex_value(value, size):
    if not isinstance(value, str) or len(value) != size * 2:
        raise TransportError("Invalid hexadecimal identity")
    try:
        decoded = bytes.fromhex(value)
    except ValueError as error:
        raise TransportError("Invalid hexadecimal identity") from error
    if len(decoded) != size:
        raise TransportError("Invalid identity length")
    return decoded


class BoundedUnits:
    def __init__(self):
        self.queue = queue.Queue(maxsize=2)
        self.lock = threading.Lock()
        self.condition = threading.Condition(self.lock)
        self.bytes = 0
        self.high_water = 0
        self.completed = 0
        self.startup_until = time.monotonic() + 2

    def put(self, data, timeout=None):
        if len(data) > 4 * 1024 * 1024:
            raise TransportError("Encoded access unit exceeds queue budget")
        with self.condition:
            # Allow two seconds for cold decoder/viewer startup. This is an
            # absolute grace window, not renewed by queue progress. The two
            # pending-frame/4 MiB budgets remain; steady-state waits are 150 ms.
            deadline = time.monotonic() + (timeout if timeout is not None else
                                           (max(0.15, self.startup_until - time.monotonic())))
            while self.bytes + len(data) > 4 * 1024 * 1024 or self.queue.full():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TransportError(f"Encoded queue congested: {self.queue.qsize()} pending, {self.bytes} bytes; refusing dependent-frame drops")
                self.condition.wait(remaining)
            self.queue.put_nowait(data)
            self.bytes += len(data)
            self.high_water = max(self.high_water, self.bytes)

    def get(self, timeout=0):
        return self.queue.get(timeout=timeout)

    def release(self, data):
        with self.condition:
            self.bytes -= len(data)
            self.completed += 1
            self.condition.notify_all()


def exact(stream, count):
    data = bytearray()
    while len(data) < count:
        chunk = stream.read(count - len(data))
        if not chunk:
            if data:
                raise TransportError("Truncated source access unit")
            return None
        data.extend(chunk)
    return bytes(data)


class LogEvents:
    """Read child diagnostic events without moving its shared file offset."""
    def __init__(self, stream):
        self.stream, self.offset, self.partial = stream, 0, b""

    def poll(self):
        data = os.pread(self.stream.fileno(), 16384, self.offset)
        self.offset += len(data)
        lines = (self.partial + data).split(b"\n")
        self.partial = lines.pop()
        if len(self.partial) > 16384:
            raise TransportError("Child diagnostic line exceeds budget")
        events = []
        for line in lines:
            if not line.startswith(b'{'):
                continue
            try:
                value = json.loads(line)
                if isinstance(value, dict) and isinstance(value.get("event"), str):
                    events.append(value)
            except ValueError:
                pass  # Final pretty-printed diagnostics are not event records.
        return events


class Source:
    def __init__(self, args):
        self.units = BoundedUnits()
        self.finished = threading.Event()
        self.stopping = threading.Event()
        self.error = None
        self.log = tempfile.TemporaryFile()
        self.events = LogEvents(self.log)
        self.source = args.source
        if args.source == "test":
            command = ["ffmpeg", "-v", "error", "-re", "-f", "lavfi", "-i",
                       "testsrc2=size=1920x1080:rate=30", "-t", str(args.duration),
                       "-an", "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
                       "-profile:v", "baseline", "-pix_fmt", "yuv420p", "-bf", "0", "-g", "30",
                       "-x264-params", "aud=1:repeat-headers=1", "-b:v", "12000000", "-f", "h264", "pipe:1"]
            framed = False
        elif args.source == "gnome":
            command = [sys.executable, str(Path(__file__).with_name("virtual_display_probe.py")),
                       "--exercise-gnome", "--duration", str(args.duration), "--framed-h264-stdout",
                       "--parent-pid", str(os.getpid())]
            framed = True
        elif args.source == "macos":
            if not args.macos_probe or not Path(args.macos_probe).is_file():
                raise TransportError("Specify the built native probe using --macos-probe")
            command = [args.macos_probe, "--exercise", "--duration", str(args.duration), "--framed-h264-stdout",
                       "--parent-pid", str(os.getpid())]
            framed = True
        else:
            raise TransportError("Unsupported source; no fallback to another display")
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=self.log, bufsize=0)
        self.thread = threading.Thread(target=self._read, args=(framed,), daemon=True)
        self.thread.start()

    def _read(self, framed):
        try:
            if framed:
                while True:
                    size = exact(self.process.stdout, 4)
                    if size is None:
                        break
                    count = struct.unpack("!I", size)[0]
                    if not 0 < count <= MAX_AU:
                        raise TransportError("Source frame exceeds budget")
                    data = exact(self.process.stdout, count)
                    if data is None:
                        raise TransportError("Source ended within access unit")
                    if not self.stopping.is_set():
                        nal_types(data)
                        self.units.put(data)
            else:
                splitter = AccessUnitSplitter()
                while True:
                    chunk = os.read(self.process.stdout.fileno(), 16384)
                    if not chunk:
                        break
                    for data in splitter.feed(chunk):
                        if not self.stopping.is_set():
                            self.units.put(data)
                for data in splitter.finish():
                    if not self.stopping.is_set():
                        self.units.put(data)
            code = self.process.wait(timeout=15)
            if code and not self.stopping.is_set():
                self.log.seek(0)
                raise TransportError("Capture helper failed: " + self.log.read(2000).decode(errors="replace"))
        except Exception as error:
            if not self.stopping.is_set():
                self.error = str(error)
        finally:
            self.process.stdout.close()
            self.finished.set()

    def close(self):
        self.stopping.set()
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)
        self.thread.join(timeout=3)
        self.process.stdout.close()
        self.log.seek(0)
        diagnostics = self.log.read(16384).decode(errors="replace")
        self.log.close()
        result = {}
        if self.source != "test":
            try:
                start = diagnostics.rfind("\n{")
                result = json.loads(diagnostics[start+1:] if start >= 0 else diagnostics)
            except (ValueError, TypeError):
                result = {"cleanupVerified": False, "error": "Missing capture-helper cleanup report"}
            print(json.dumps({"event": "sourceStopped", "source": self.source,
                              "cleanupVerified": result.get("cleanupVerified", False),
                            "hardwareEncoder": result.get("hardwareEncoder"), "frames": result.get("frames"),
                              "encodedFrames": result.get("encodedFrames"),
                              "droppedRawFrames": result.get("droppedRawFrames"),
                              "targetBitrate": result.get("targetBitrate"),
                              "maximumBytesPerSecond": result.get("maximumBytesPerSecond"),
                              "diagnostics": diagnostics[-2000:] if not result.get("cleanupVerified") else None}), file=sys.stderr, flush=True)
        return result


class Decoder:
    def __init__(self, view, macos_viewer=None):
        self.viewer_process = None
        self.native_frames = 0
        if macos_viewer and not Path(macos_viewer).is_file():
            raise TransportError("Native viewer executable does not exist")
        self.units = BoundedUnits()
        self.done = threading.Event()
        self.error = None
        self.frames = 0
        self.first_decoded_at = None
        self.view = view
        self.closed = False
        self.log = tempfile.TemporaryFile()
        self.events = LogEvents(self.log)
        if view and not macos_viewer:
            command = ["ffplay", "-v", "error", "-autoexit", "-probesize", "32", "-analyzeduration", "0",
                       "-flags", "low_delay", "-framedrop", "-window_title", "Deskflow virtual display (experimental)",
                       "-f", "h264", "-i", "pipe:0"]
        else:
            command = ["ffmpeg", "-v", "error", "-xerror", "-err_detect", "explode", "-probesize", "32",
                       "-analyzeduration", "0", "-flags", "low_delay", "-f", "h264", "-i", "pipe:0",
                       "-progress", "pipe:1", "-stats_period", "0.1", "-f", "null", "-"]
        if macos_viewer:
            # The native sink receives fixed BGRA frames locally. Encoded H.264
            # remains the network format; FFmpeg supplies experimental decoding.
            command = command[:command.index("-progress")] + ["-pix_fmt", "bgra", "-threads", "1", "-f", "rawvideo", "pipe:1"]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE if not view or macos_viewer else subprocess.DEVNULL,
                                        stderr=self.log, bufsize=0)
        if macos_viewer:
            try:
                viewer_environment = os.environ.copy()
                viewer_environment['DESKFLOW_VIEWER_PARENT_PID'] = str(os.getpid())
                # XWayland exposes confirmed minimize/restore state. Native
                # Wayland cannot promise that state through xdg-shell.
                if sys.platform.startswith('linux') and viewer_environment.get('DISPLAY') and not viewer_environment.get('QT_QPA_PLATFORM'):
                    viewer_environment['QT_QPA_PLATFORM'] = 'xcb'
                self.viewer_process = subprocess.Popen([macos_viewer], stdin=self.process.stdout,
                                                       stdout=subprocess.DEVNULL, stderr=self.log,
                                                       env=viewer_environment)
            except Exception:
                self.process.kill()
                self.process.communicate(timeout=5)
                self.log.close()
                raise
            self.process.stdout.close()
        self.thread = threading.Thread(target=self._write, daemon=True)
        self.thread.start()
        self.progress_thread = None
        if not view:
            self.progress_thread = threading.Thread(target=self._progress, daemon=True)
            self.progress_thread.start()

    def _progress(self):
        for line in self.process.stdout:
            if line.startswith(b"frame="):
                try:
                    self.frames = int(line.partition(b"=")[2])
                    if self.frames > 0 and self.first_decoded_at is None:
                        self.first_decoded_at = time.monotonic()
                except ValueError:
                    self.error = "Invalid decoder progress"

    def _write(self):
        try:
            while not self.done.is_set() or not self.units.queue.empty():
                try:
                    data = self.units.get(0.1)
                except queue.Empty:
                    continue
                try:
                    pending = memoryview(data)
                    while pending:
                        count = os.write(self.process.stdin.fileno(), pending)
                        if count <= 0:
                            raise TransportError("Decoder write stalled")
                        pending = pending[count:]
                finally:
                    self.units.release(data)
        except Exception as error:
            self.error = str(error)
        finally:
            try:
                self.process.stdin.close()
            except OSError:
                pass

    def close(self, abort=False):
        # A product stop can coincide with the media STOP drain. Do not let
        # SIGTERM interrupt cleanup after closed is set but before wait() ends.
        previous = signal.signal(signal.SIGTERM, signal.SIG_IGN)
        try:
            self._close(abort)
        finally:
            signal.signal(signal.SIGTERM, previous)

    def _close(self, abort=False):
        if self.closed:
            return
        self.closed = True
        self.done.set()
        if abort and self.viewer_process and self.viewer_process.poll() is None:
            self.viewer_process.terminate()
        if abort and self.process.poll() is None:
            self.process.terminate()
        self.thread.join(timeout=7)
        if self.thread.is_alive():
            self.process.kill()
            self.thread.join(timeout=3)
            self.error = "Decoder writer timeout"
        try:
            code = self.process.wait(timeout=7)
        except subprocess.TimeoutExpired:
            self.process.kill()
            code = self.process.wait(timeout=3)
            self.error = "Decoder shutdown timeout"
        if self.viewer_process:
            try:
                viewer_code = self.viewer_process.wait(timeout=7)
            except subprocess.TimeoutExpired:
                self.viewer_process.kill()
                viewer_code = self.viewer_process.wait(timeout=3)
                self.error = "Native viewer shutdown timeout"
            if viewer_code and not abort:
                self.error = "Native viewer failed"
        if self.progress_thread:
            self.progress_thread.join(timeout=3)
            self.process.stdout.close()
        self.log.seek(0)
        detail = self.log.read(2000).decode(errors="replace")
        self.log.close()
        if self.viewer_process:
            for line in detail.splitlines():
                if line.startswith('{'):
                    event = json.loads(line)
                    if event.get("event") == "nativeViewerStopped":
                        self.native_frames = event.get("submittedFrames", 0)
        if code and not abort:
            self.error = "Decoder failed: " + detail


def child_connect(args, context, channel, session, epoch, token):
    wire = authenticate(context, socket.create_connection((args.host, args.port), timeout=4), args.peer_cert)
    result = FramedSocket(wire, channel, session, epoch)
    try:
        result.send(BIND, {"channel": channel, "token": token})
        reply = require_fields(result.json(BIND), ("channel", "bound"))
        if reply != {"channel": channel, "bound": True}:
            raise TransportError("Channel binding rejected")
        return result
    except Exception:
        result.close()
        raise


def serve(args):
    context = tls_context(args.identity, args.peer_cert, True)
    listener = socket.socket(socket.AF_INET6 if ":" in args.bind else socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.bind, args.port))
    listener.listen(3)
    listener.settimeout(60)
    print(json.dumps({"event": "listening", "port": listener.getsockname()[1]}), file=sys.stderr, flush=True)
    channels = []
    tokens = None
    source = None
    report = {"role": "host", "sentFrames": 0, "inputEnabled": False, "source": args.source, "firstFramePresented": False}
    try:
        raw, _ = listener.accept()
        control = FramedSocket(authenticate(context, raw, args.peer_cert, True), "control")
        channels.append(control)
        hello_frame = control.receive()
        if hello_frame.kind != HELLO or hello_frame.session != ZERO or hello_frame.epoch != 0:
            raise TransportError("Invalid initial hello")
        hello = require_fields(decode_json(hello_frame.payload), ("role", "nonce", "codec", "width", "height", "fps"))
        hex_value(hello["nonce"], 32)
        if hello["role"] != "receiver" or hello["codec"] != "h264":
            raise TransportError("Unsupported role or codec")
        if (integer(hello["width"], 16, 1920), integer(hello["height"], 16, 1080),
                integer(hello["fps"], 1, 60)) != (1920, 1080, 30):
            raise TransportError("This media adapter currently requires 1080p30")
        session = hex_value(args.session_id, 16) if getattr(args, "session_id", None) else secrets.token_bytes(16)
        epoch = 1
        control.bind(session, epoch)
        control.send(CAPS, {"session": session.hex(), "epoch": epoch, "nonce": secrets.token_hex(32),
                            "codec": "h264", "width": 1920, "height": 1080, "fps": 30, "input": False})
        start = require_fields(control.json(START), ("requestId",))
        hex_value(start["requestId"], 16)
        if getattr(args, "request_id", None) and start["requestId"] != args.request_id:
            raise TransportError("Product request binding mismatch")
        tokens = BindingTokens(session, fingerprint(args.peer_cert), epoch)
        control.send(ACCEPT, {"requestId": start["requestId"], "tokens": tokens.tokens.copy(), "input": False})
        bound = {}
        for _ in range(8):
            if len(bound) == 2:
                break
            listener.settimeout(max(0.1, tokens.deadline - time.monotonic()))
            raw, _ = listener.accept()
            child = None
            try:
                wire = authenticate(context, raw, args.peer_cert, True)
                # First bind is JSON and has the same envelope on both child channels.
                child = FramedSocket(wire, "video", session, epoch)
                binding = require_fields(child.json(BIND), ("channel", "token"))
                name = binding["channel"]
                if name not in ("input", "video") or name in bound:
                    raise TransportError("Invalid child channel")
                tokens.consume(name, binding["token"], session, fingerprint(args.peer_cert), epoch)
                child.reader.channel = name
                child.send(BIND, {"channel": name, "bound": True})
                bound[name] = child
                channels.append(child)
            except Exception:
                (child.close() if child else raw.close())
                raise
        if len(bound) != 2:
            raise TransportError("Missing child channel")
        listener.close()
        control.send(CONFIG, {"codec": "h264", "width": 1920, "height": 1080, "fps": 30,
                              "format": "annexb-au", "captureTimestamps": False, "input": False})
        if require_fields(control.json(READY), ("ready",)) != {"ready": True}:
            raise TransportError("Receiver not ready")
        if getattr(args, "product_events", False):
            print(json.dumps({"event":"streamConfiguration","config":{"codec":"h264","width":1920,"height":1080,"fps":30,"input":False,"color":"SDR"}}), file=sys.stderr, flush=True)
        source = Source(args)
        first = True
        last_pong = time.monotonic()
        last_ping = 0
        pending_pings = {}
        metrics = TransferMetrics(channels)
        deadline = time.monotonic() + args.duration + 15 if args.duration else float("inf")
        while time.monotonic() < deadline:
            measured = metrics.sample()
            if measured is not None and getattr(args, "product_events", False):
                print(json.dumps({"event":"streamStatistics","statistics":measured}, allow_nan=False), file=sys.stderr, flush=True)
            for event in source.events.poll():
                if event.get("event") == "displayCreated" and getattr(args, "product_events", False):
                    print(json.dumps(event), file=sys.stderr, flush=True)
            if source.error:
                raise TransportError(source.error)
            now = time.monotonic()
            if now - last_pong > 3:
                raise TransportError("Control heartbeat lost")
            if now - last_ping >= 1:
                nonce = secrets.token_hex(8)
                pending_pings[nonce] = now
                control.send(PING, {"nonce": nonce})
                last_ping = now
            try:
                data = source.units.get(0.005)
            except queue.Empty:
                data = None
            if data:
                try:
                    kinds = nal_types(data)
                    if first and not {5, 7, 8} <= set(kinds):
                        raise TransportError("First access unit must contain SPS, PPS and IDR")
                    first = False
                    before_send = time.monotonic()
                    bound["video"].send(VIDEO, payload=struct.pack("!QB", 0, int(5 in kinds)) + data)
                    elapsed = round((time.monotonic() - before_send) * 1000, 2)
                    report["maxFrameSendMs"] = max(report.get("maxFrameSendMs", 0), elapsed)
                    report["sentFrames"] += 1
                finally:
                    source.units.release(data)
            elif source.finished.is_set():
                if source.error:
                    raise TransportError(source.error)
                break
            try:
                if not control.readable():
                    raise TimeoutError()
                frame = control.receive(0.005)
                value = decode_json(frame.payload)
                if frame.kind == PONG:
                    require_fields(value, ("nonce",))
                    if value["nonce"] not in pending_pings:
                        raise TransportError("Unexpected heartbeat response")
                    metrics.record_rtt((time.monotonic() - pending_pings.pop(value["nonce"])) * 1000)
                    last_pong = time.monotonic()
                elif frame.kind == PRESENT:
                    require_fields(value, ("presented", "path"))
                    if value != {"presented": True, "path": "native"} or report["firstFramePresented"]:
                        raise TransportError("Invalid or duplicate presentation acknowledgment")
                    report["firstFramePresented"] = True
                    if getattr(args, "product_events", False):
                        print(json.dumps({"event": "firstFramePresented", "path": "authenticated-peer"}), file=sys.stderr, flush=True)
                elif frame.kind == STATS:
                    require_fields(value, ("decodedFrames",))
                    report["decodedBeforeStop"] = integer(value["decodedFrames"], 0, 2**32-1)
                elif frame.kind == STOP:
                    require_fields(value, ("reason",))
                    raise TransportError("Receiver stopped: " + str(value["reason"])[:128])
                else:
                    raise TransportError("Unexpected control message")
            except TimeoutError:
                pass
            try:
                if not bound["input"].readable():
                    raise TimeoutError()
                frame = bound["input"].receive(0.001)
                if frame.kind != RELEASE or frame.payload != b"{}":
                    raise TransportError("Input injection is disabled")
            except TimeoutError:
                pass
        else:
            raise TransportError("Capture deadline exceeded")
        if first:
            raise TransportError("Capture produced no usable access unit")
        control.send(STOP, {"reason": "source_complete", "lastVideoSequence": bound["video"].sequence})
        stop_deadline = time.monotonic() + 10
        while True:
            frame = control.receive(max(0.1, stop_deadline - time.monotonic()))
            value = decode_json(frame.payload)
            if frame.kind == STOPPED:
                require_fields(value, ("receivedFrames", "decodedFrames"))
                report["receiver"] = {key: integer(value[key], 0, 2**32-1) for key in value}
                break
            if frame.kind == PRESENT and not report["firstFramePresented"]:
                require_fields(value, ("presented", "path"))
                if value != {"presented": True, "path": "native"}:
                    raise TransportError("Invalid final presentation acknowledgment")
                report["firstFramePresented"] = True
                if getattr(args, "product_events", False):
                    print(json.dumps({"event": "firstFramePresented", "path": "authenticated-peer"}), file=sys.stderr, flush=True)
                continue
            if frame.kind not in (PONG, STATS) or time.monotonic() >= stop_deadline:
                raise TransportError("Receiver did not finish")
        report.update(tls="TLSv1.3", sourceQueueHighWater=source.units.high_water, passed=True)
        return report
    except Exception:
        print(json.dumps({"event": "sessionFailed", "sentFrames": report["sentFrames"],
                          "maxFrameSendMs": report.get("maxFrameSendMs"),
                          "sourceQueueHighWater": source.units.high_water if source else 0}),
              file=sys.stderr, flush=True)
        raise
    finally:
        if tokens:
            tokens.revoke()
        for channel in channels:
            channel.close()
        listener.close()
        if source:
            cleanup = source.close()
            if args.source != "test":
                report["sourceCleanupVerified"] = cleanup.get("cleanupVerified", False)
                if not report["sourceCleanupVerified"]:
                    report["passed"] = False
                    report["error"] = "Source cleanup could not be verified"


def receive(args):
    context = tls_context(args.identity, args.peer_cert, False)
    channels = []
    decoder = None
    clean = False
    report = {"role": "receiver", "receivedFrames": 0, "view": args.view, "inputEnabled": False,
              "presentationVerified": False}
    try:
        raw = socket.create_connection((args.host, args.port), timeout=4)
        control = FramedSocket(authenticate(context, raw, args.peer_cert), "control")
        channels.append(control)
        control.send(HELLO, {"role": "receiver", "nonce": secrets.token_hex(32), "codec": "h264",
                             "width": 1920, "height": 1080, "fps": 30})
        caps_frame = control.receive()
        if caps_frame.kind != CAPS:
            raise TransportError("Missing capabilities")
        caps = require_fields(decode_json(caps_frame.payload), ("session", "epoch", "nonce", "codec", "width", "height", "fps", "input"))
        session = hex_value(caps["session"], 16)
        epoch = integer(caps["epoch"], 1, 2**32-1)
        hex_value(caps["nonce"], 32)
        if caps_frame.session != session or caps_frame.epoch != epoch or caps["input"] is not False:
            raise TransportError("Capabilities binding mismatch")
        if (caps["codec"], caps["width"], caps["height"], caps["fps"]) != ("h264", 1920, 1080, 30):
            raise TransportError("Unsupported media configuration")
        if getattr(args, "session_id", None) and session != hex_value(args.session_id, 16):
            raise TransportError("Product session binding mismatch")
        control.bind(session, epoch)
        request = args.request_id if getattr(args, "request_id", None) else secrets.token_hex(16)
        control.send(START, {"requestId": request})
        accept = require_fields(control.json(ACCEPT), ("requestId", "tokens", "input"))
        require_fields(accept["tokens"], ("video", "input"))
        if accept["requestId"] != request or accept["input"] is not False:
            raise TransportError("Invalid session acceptance")
        for token in accept["tokens"].values():
            hex_value(token, 32)
        video = child_connect(args, context, "video", session, epoch, accept["tokens"]["video"])
        channels.append(video)
        inputs = child_connect(args, context, "input", session, epoch, accept["tokens"]["input"])
        channels.append(inputs)
        config = require_fields(control.json(CONFIG), ("codec", "width", "height", "fps", "format", "captureTimestamps", "input"))
        if config != {"codec": "h264", "width": 1920, "height": 1080, "fps": 30,
                       "format": "annexb-au", "captureTimestamps": False, "input": False}:
            raise TransportError("Unsupported stream config")
        if getattr(args, "product_events", False):
            print(json.dumps({"event":"streamConfiguration","config":dict(config, color="SDR")}), file=sys.stderr, flush=True)
        decoder = Decoder(args.view, args.macos_viewer)
        if getattr(args, "product_events", False):
            print(json.dumps({"event": "receiverPrepared"}), file=sys.stderr, flush=True)
        control.send(READY, {"ready": True})
        last_control = time.monotonic()
        last_stats = 0
        metrics = TransferMetrics(channels)
        last_sequence = 1  # Bind acknowledgment.
        expected_last = None
        stopped_at = None
        first = True
        report["firstFramePresented"] = False
        while True:
            measured = metrics.sample()
            if measured is not None and getattr(args, "product_events", False):
                print(json.dumps({"event":"streamStatistics","statistics":measured}, allow_nan=False), file=sys.stderr, flush=True)
            for event in decoder.events.poll():
                if event.get("event") == "firstFramePresented" and event.get("path") in ("QWidget.paintEvent", "NSView.drawRect"):
                    if not report["firstFramePresented"]:
                        report["firstFramePresented"] = True
                        control.send(PRESENT, {"presented": True, "path": "native"})
                        if getattr(args, "product_events", False):
                            print(json.dumps(event), file=sys.stderr, flush=True)
                elif event.get("event") in ("viewerClosed", "receiverWindowState") and getattr(args, "product_events", False):
                    print(json.dumps(event), file=sys.stderr, flush=True)
            now = time.monotonic()
            if now - last_control > 4:
                raise TransportError("Control connection lost")
            if decoder.error or decoder.process.poll() is not None:
                raise TransportError(decoder.error or "Decoder or viewer exited")
            try:
                if not control.readable():
                    raise TimeoutError()
                frame = control.receive(0.005)
                value = decode_json(frame.payload)
                last_control = time.monotonic()
                if frame.kind == PING:
                    require_fields(value, ("nonce",))
                    hex_value(value["nonce"], 8)
                    control.send(PONG, value)
                elif frame.kind == STOP:
                    require_fields(value, ("reason", "lastVideoSequence"))
                    expected_last = integer(value["lastVideoSequence"], 2, 2**64-1)
                    stopped_at = time.monotonic()
                else:
                    raise TransportError("Unexpected host control message")
            except TimeoutError:
                pass
            try:
                frame = video.receive(0.02)
                if frame.kind != VIDEO or len(frame.payload) <= 9 or frame.payload[8] & ~1:
                    raise TransportError("Invalid video access unit")
                data = frame.payload[9:]
                kinds = nal_types(data)
                if bool(frame.payload[8] & 1) != (5 in kinds) or (first and not {5, 7, 8} <= set(kinds)):
                    raise TransportError("Invalid IDR flag or missing initial configuration")
                if first:
                    print(json.dumps({"event": "firstVideo"}), file=sys.stderr, flush=True)
                first = False
                decoder.units.put(data)
                report["receivedFrames"] += 1
                last_sequence = frame.sequence
            except TimeoutError:
                pass
            if now - last_stats > 0.5 and expected_last is None:
                control.send(STATS, {"decodedFrames": decoder.frames})
                last_stats = now
            if expected_last is not None:
                if last_sequence > expected_last:
                    raise TransportError("Video continues beyond declared stop sequence")
                if last_sequence == expected_last:
                    break
        decoder.close()
        if decoder.error or (not args.view and decoder.frames < 1):
            raise TransportError(decoder.error or "No frame was decoded")
        if args.view:
            report["viewerExitedCleanly"] = True
            if args.macos_viewer:
                report["nativeSubmittedFrames"] = decoder.native_frames
        control.send(STOPPED, {"receivedFrames": report["receivedFrames"], "decodedFrames": decoder.frames})
        report.update(decodedFrames=decoder.frames, decodedWhileStreaming=decoder.first_decoded_at is not None and
                      decoder.first_decoded_at < stopped_at,
                      decoderQueueHighWater=decoder.units.high_water, tls="TLSv1.3", passed=True)
        clean = True
        return report
    finally:
        for channel in channels:
            channel.close()
        if decoder and not clean:
            decoder.close(abort=True)
        if decoder and getattr(args, "product_events", False):
            verified = decoder.process.poll() is not None and (not decoder.viewer_process or decoder.viewer_process.poll() is not None)
            print(json.dumps({"event": "receiverStopped", "cleanupVerified": verified}), file=sys.stderr, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    identity = actions.add_parser("init-identity")
    identity.add_argument("--directory", type=Path, required=True)
    for action in ("serve", "receive"):
        sub = actions.add_parser(action)
        sub.add_argument("--identity", type=Path, required=True)
        sub.add_argument("--peer-cert", type=Path, required=True)
        sub.add_argument("--port", type=int, default=24810)
        sub.add_argument("--session-id", help="Authorized product session identity")
        sub.add_argument("--request-id", help="Authorized product request identity")
        sub.add_argument("--product-events", action="store_true", help="Emit bounded product adapter lifecycle events")
        sub.add_argument("--parent-pid", type=int, help="End the session if the owning product process exits")
        if action == "serve":
            sub.add_argument("--bind", default="127.0.0.1")
            sub.add_argument("--source", choices=("gnome", "macos", "test"), required=True)
            sub.add_argument("--duration", type=int, default=10)
            sub.add_argument("--macos-probe")
        else:
            sub.add_argument("--host", default="127.0.0.1")
            sub.add_argument("--view", action="store_true", help="Open an ffplay window; no synthetic PresentAck")
            sub.add_argument("--macos-viewer", "--native-viewer", dest="macos_viewer", help="Built native BGRA viewer; implies --view")
    args = parser.parse_args()
    if args.action != "init-identity":
        if bool(args.session_id) != bool(args.request_id):
            parser.error("Product session and request identities must be supplied together")
        if args.session_id:
            try:
                if hex_value(args.session_id, 16) == ZERO or hex_value(args.request_id, 16) == ZERO:
                    raise TransportError("Zero product identity")
            except TransportError as error:
                parser.error(str(error))
    if args.action == "receive" and args.macos_viewer:
        args.view = True
    if args.action != "init-identity" and not (1 <= args.port <= 65535 or
            (args.action == "serve" and args.port == 0 and args.bind in ("127.0.0.1", "::1"))):
        parser.error("Port out of range")
    if args.action == "serve" and not (1 <= args.duration <= 3600 or (args.duration == 0 and args.product_events and args.parent_pid and args.source != "test")):
        parser.error("Duration must be 1..3600 seconds, or 0 for a product-owned native source")
    def interrupted(_signal, _frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    watch_done = threading.Event()
    if args.action != "init-identity" and args.parent_pid is not None:
        if args.parent_pid < 1 or os.getppid() != args.parent_pid:
            parser.error("Owning product process is no longer available")
        def watch_owner():
            while not watch_done.wait(0.1):
                if os.getppid() != args.parent_pid:
                    os.kill(os.getpid(), signal.SIGTERM)
                    break
        threading.Thread(target=watch_owner, daemon=True).start()
    try:
        if args.action == "init-identity":
            report = {"fingerprint": init_identity(args.directory), "passed": True}
        else:
            report = serve(args) if args.action == "serve" else receive(args)
    except (Exception, KeyboardInterrupt) as error:
        report = {"passed": False, "error": str(error) or "Interrupted"}
    finally:
        watch_done.set()
    print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)
    return 0 if report.get("passed") else 1


if __name__ == "__main__":
    sys.exit(main())
