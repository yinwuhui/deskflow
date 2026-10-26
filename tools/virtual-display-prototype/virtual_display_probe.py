#!/usr/bin/env python3
"""Read-only capability report, or an explicit temporary GNOME virtual-output test.

SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
SPDX-License-Identifier: MIT
"""

import argparse
import json
import os
import platform
import queue
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


def command(args, timeout=8):
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return {"exitCode": result.returncode, "stdout": result.stdout[-16000:],
                "stderr": result.stderr[-2000:]}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"exitCode": None, "error": type(error).__name__}


def doctor():
    report = {"schemaVersion": 1, "platform": platform.system(),
              "platformRelease": platform.release(), "architecture": platform.machine(),
              "observations": {}, "certified": False,
              "note": "Discovery is not proof of display creation, capture, encoding or cleanup."}
    observations = report["observations"]
    system = platform.system()
    if system == "Linux":
        observations["sessionEnvironment"] = {
            key: os.environ.get(key) for key in
            ("XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "WAYLAND_DISPLAY", "DISPLAY")}
        observations["sessionEnvironmentNote"] = "SSH environment may differ from the desktop owning the user bus."
        observations["portalSourceTypes"] = command([
            "gdbus", "call", "--session", "--dest", "org.freedesktop.portal.Desktop",
            "--object-path", "/org/freedesktop/portal/desktop", "--method",
            "org.freedesktop.DBus.Properties.Get", "org.freedesktop.portal.ScreenCast",
            "AvailableSourceTypes"])
        observations["mutterScreenCast"] = command([
            "gdbus", "introspect", "--session", "--dest", "org.gnome.Mutter.ScreenCast",
            "--object-path", "/org/gnome/Mutter/ScreenCast"])
        observations["pipewireSource"] = command(["gst-inspect-1.0", "pipewiresrc"])
        observations["ffmpegEncoders"] = command(["ffmpeg", "-hide_banner", "-encoders"])
    elif system == "Darwin":
        observations["osVersion"] = command(["sw_vers"])
        observations["displays"] = command(["system_profiler", "SPDisplaysDataType", "-json"], 20)
        observations["virtualDisplay"] = "Not exercised: CGVirtualDisplay requires the native private-API adapter."
    elif system == "Windows":
        observations["displayDevices"] = command([
            "powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
            "Get-CimInstance Win32_PnPSignedDriver | Where-Object DeviceClass -eq 'DISPLAY' | "
            "Select-Object DeviceName,DriverVersion,IsSigned | ConvertTo-Json -Compress"], 20)
        observations["virtualDisplay"] = "Not exercised: a compatible, signed IDD and its management interface are required."
    else:
        report["unsupportedPlatform"] = True
    observations["tools"] = {name: bool(shutil.which(name)) for name in
                             ("ffmpeg", "gst-launch-1.0", "gdbus", "cmake")}
    return report


def exercise_gnome(width, height, fps, duration, h264_output=None, h264_stream=False, parent_pid=None):
    # Imports are deliberately optional for read-only diagnosis on other platforms.
    import gi
    gi.require_version("Gst", "1.0")
    from gi.repository import GLib, Gst
    import dbus
    from dbus.mainloop.glib import DBusGMainLoop

    DBusGMainLoop(set_as_default=True)
    Gst.init(None)
    bus = dbus.SessionBus()
    service = "org.gnome.Mutter.ScreenCast"
    config = dbus.Interface(bus.get_object("org.gnome.Mutter.DisplayConfig",
                                          "/org/gnome/Mutter/DisplayConfig"),
                            "org.gnome.Mutter.DisplayConfig")

    def connectors():
        state = config.GetCurrentState(timeout=5)
        return sorted(str(monitor[0][0]) for monitor in state[1])

    def logical_displays():
        state = config.GetCurrentState(timeout=5)
        return [{"x": int(item[0]), "y": int(item[1]), "scale": float(item[2]),
                 "connectors": [str(spec[0]) for spec in item[5]]} for item in state[2]]

    # Commands are handled inside the process retaining this ScreenCast session.
    # A connector name from a journal is never sufficient authority to mutate it.
    control_directory = os.environ.get("DESKFLOW_LAYOUT_DIRECTORY", "")
    control_token = os.environ.get("DESKFLOW_LAYOUT_TOKEN", "")
    last_command = None

    def topology(owned, request=None, allow_rollback=True):
        state = config.GetCurrentState(timeout=3)
        primary = next((item for item in state[2] if item[4]), None)
        if primary is None:
            raise RuntimeError("Primary display unavailable")
        origin_x, origin_y = int(primary[0]), int(primary[1])
        monitors = {str(item[0][0]): item for item in state[1]}
        displays = []
        configurations = []
        target = None
        for logical in state[2]:
            x, y, scale, transform, is_primary, specs, properties = logical
            entries = []
            for spec in specs:
                connector = str(spec[0])
                monitor = monitors[connector]
                mode = next((mode for mode in monitor[1] if mode[6].get("is-current", False)), None)
                if mode is None:
                    raise RuntimeError("Current display mode unavailable")
                entries.append((connector, str(mode[0]), dbus.Dictionary({}, signature="sv")))
                width, height = int(mode[1]), int(mode[2])
                if int(transform) % 2:
                    width, height = height, width
                item = dict(displayId=connector, x=int(x)-origin_x, y=int(y)-origin_y,
                            width=round(width/float(scale)), height=round(height/float(scale)),
                            pixelWidth=int(mode[1]), pixelHeight=int(mode[2]), scale=float(scale),
                            primary=bool(is_primary), owned=connector == owned)
                displays.append(item)
                if connector == owned:
                    if len(specs) != 1 or is_primary:
                        raise RuntimeError("Owned display is mirrored or primary")
                    target = item
            configurations.append([int(x), int(y), float(scale), int(transform), bool(is_primary), entries])
        if target is None:
            raise RuntimeError("Owned display no longer exists")
        if request is not None:
            if request.get("scale", target["scale"]) != target["scale"]:
                raise ValueError("Scale is read-only")
            for logical in configurations:
                if any(entry[0] == owned for entry in logical[5]):
                    logical[0], logical[1] = request["x"]+origin_x, request["y"]+origin_y
            min_x = min(item[0] for item in configurations)
            min_y = min(item[1] for item in configurations)
            for logical in configurations:
                logical[0] -= min_x
                logical[1] -= min_y
            properties = dbus.Dictionary({}, signature="sv")
            if "layout-mode" in state[3]:
                properties["layout-mode"] = state[3]["layout-mode"]
            config.ApplyMonitorsConfig(state[0], dbus.UInt32(1),
                dbus.Array([dbus.Struct(item, signature="iiduba(ssa{sv})") for item in configurations],
                           signature="(iiduba(ssa{sv}))"), properties, timeout=3)
            confirmed = topology(owned)
            if any(confirmed["layout"][key] != request[key] for key in ("x", "y")):
                if allow_rollback:
                    topology(owned, {"x": target["x"], "y": target["y"]}, False)
                raise RuntimeError("System did not confirm requested position")
            return confirmed
        return dict(coordinateSpace="primary-relative-logical", displays=displays, layout=target,
                    capabilities=dict(position=True, scale=False))

    def layout_commands(owned):
        nonlocal last_command
        if not control_directory or not control_token:
            return
        path = Path(control_directory)
        request_file = path / "request.json"
        if not request_file.exists() or request_file.stat().st_size > 4096:
            return
        try:
            request = json.loads(request_file.read_text())
            if request.get("token") != control_token or request.get("id") == last_command:
                return
            last_command = request["id"]
            result = dict(token=control_token, id=last_command, accepted=False)
            if request.get("operation") not in ("QueryDisplayLayout", "SetDisplayLayout"):
                raise ValueError("Unknown layout operation")
            layout = request.get("layout") if request["operation"] == "SetDisplayLayout" else None
            if layout is not None:
                if set(layout)-{"x", "y", "scale"} or not all(type(layout.get(k)) is int and abs(layout[k]) <= 32768 for k in ("x", "y")):
                    raise ValueError("Invalid layout")
            result["data"] = topology(owned, layout)
            result["accepted"] = True
        except Exception as error:
            if last_command is None:
                return
            result = dict(token=control_token, id=last_command, accepted=False, error=str(error))
        temporary = path / "response.tmp"
        temporary.write_text(json.dumps(result, separators=(",", ":")))
        temporary.replace(path / "response.json")

    report = {"schemaVersion": 1, "backend": "gnome-mutter-private-dbus",
              "mode": {"width": width, "height": height, "fps": fps},
              "frames": 0, "created": False, "extended": False, "captured": False,
              "cleanupVerified": False, "certified": False}
    if parent_pid is not None and os.getppid() != parent_pid:
        raise RuntimeError("Parent session already exited")
    before = connectors()
    report["connectorsBefore"] = before
    loop = GLib.MainLoop()
    session = None
    pipeline = None
    matches = []
    timers = []
    signal_handlers = {}
    announced = False
    first_frame = None
    last_frame = None
    started = time.monotonic()
    encoder = None
    encoder_thread = None
    encoder_log = None
    encoded_reader = None
    encoder_queue = queue.Queue(maxsize=2)
    encoder_errors = []
    report["encoding"] = {"requested": bool(h264_output or h264_stream), "submittedFrames": 0, "droppedRawFrames": 0}

    def fail(message):
        report.setdefault("errors", []).append(str(message))
        loop.quit()
        return False

    def handoff(sink, buffer, pad):
        nonlocal first_frame, last_frame
        now = time.monotonic()
        if first_frame is None:
            first_frame = now
        last_frame = now
        report["frames"] += 1
        caps = pad.get_current_caps()
        if caps:
            report["negotiatedCaps"] = caps.to_string()
            structure = caps.get_structure(0)
            report["capturedModeMatches"] = (structure.get_value("width") == width and
                                             structure.get_value("height") == height)
        if encoder:
            if buffer.get_size() != width * height * 4:
                GLib.idle_add(fail, "Raw buffer has padding or unexpected layout; refusing corrupt encode")
                return
            raw = buffer.extract_dup(0, buffer.get_size())
            try:
                encoder_queue.put_nowait(raw)
            except queue.Full:
                try:
                    encoder_queue.get_nowait()
                    report["encoding"]["droppedRawFrames"] += 1
                except queue.Empty:
                    pass
                encoder_queue.put_nowait(raw)

    def write_frames():
        try:
            while True:
                raw = encoder_queue.get()
                if raw is None:
                    break
                pending = memoryview(raw)
                while pending:
                    size = os.write(encoder.stdin.fileno(), pending)
                    if size <= 0:
                        raise OSError("Encoder input made no progress")
                    pending = pending[size:]
                report["encoding"]["submittedFrames"] += 1
        except Exception as error:
            encoder_errors.append(str(error))
            GLib.idle_add(fail, "Encoder writer: " + str(error))
        finally:
            try:
                encoder.stdin.close()
            except OSError:
                pass

    def stream_access_units():
        from h264_access_units import AccessUnitSplitter, emit_framed
        try:
            splitter = AccessUnitSplitter()
            while True:
                data = os.read(encoder.stdout.fileno(), 16384)
                if not data:
                    break
                for unit in splitter.feed(data):
                    emit_framed(sys.stdout.buffer, unit)
            for unit in splitter.finish():
                emit_framed(sys.stdout.buffer, unit)
        except Exception as error:
            encoder_errors.append(str(error))
            GLib.idle_add(fail, "Encoder output: " + str(error))
        finally:
            encoder.stdout.close()

    def on_message(_bus, message):
        if message.type == Gst.MessageType.ERROR:
            error, detail = message.parse_error()
            fail(f"GStreamer: {error}; {detail}")
        elif message.type == Gst.MessageType.EOS:
            fail("Unexpected capture EOS")

    def on_node(node_id):
        nonlocal pipeline
        try:
            report["pipewireNodeId"] = int(node_id)
            report["pipewireFormats"] = command(["pw-cli", "enum-params", str(int(node_id)), "EnumFormat"], 3)
            pipeline = Gst.parse_launch(
                f"pipewiresrc path={int(node_id)} do-timestamp=true keepalive-time=100 ! "
                f"video/x-raw,format=BGRx,width={width},height={height},"
                f"framerate=0/1,max-framerate={fps}/1 ! "
                "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
                "fakesink name=probe_sink sync=false signal-handoffs=true")
            pipeline.get_by_name("probe_sink").connect("handoff", handoff)
            gstbus = pipeline.get_bus()
            gstbus.add_signal_watch()
            gstbus.connect("message", on_message)
            result = pipeline.set_state(Gst.State.PLAYING)
            if result == Gst.StateChangeReturn.FAILURE:
                message = gstbus.pop_filtered(Gst.MessageType.ERROR)
                if message:
                    error, detail = message.parse_error()
                    fail(f"Capture pipeline refused PLAYING: {error}; {detail}")
                else:
                    fail("Capture pipeline refused PLAYING")
        except Exception as error:
            fail(error)

    def check():
        nonlocal announced
        if parent_pid is not None and os.getppid() != parent_pid:
            return fail("Parent session exited")
        if first_frame is None:
            if time.monotonic() - started > 15:
                return fail("Timed out waiting for the first virtual-display frame")
            return True
        report["connectorsDuring"] = connectors()
        added = set(report["connectorsDuring"]) - set(before)
        report["created"] = bool(added)
        if h264_stream and len(added) == 1 and not announced:
            announced = True
            print(json.dumps({"event": "displayCreated", "virtualDisplayId": next(iter(added))}), file=sys.stderr, flush=True)
        if len(added) == 1:
            layout_commands(next(iter(added)))
        report["logicalDisplaysDuring"] = logical_displays()
        report["extended"] = any(set(item["connectors"]) <= added and item["connectors"]
                                 for item in report["logicalDisplaysDuring"])
        if duration > 0 and time.monotonic() - first_frame >= duration:
            loop.quit()
            return False
        return True

    try:
        if h264_output or h264_stream:
            # Deliberate software baseline, independent from hardware-encoder
            # discovery. Timestamp cadence is synthetic for this offline file.
            encoder_log = tempfile.TemporaryFile()
            encoder = subprocess.Popen([
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-n",
                "-f", "rawvideo", "-pixel_format", "bgr0", "-video_size", f"{width}x{height}",
                "-framerate", str(fps), "-i", "pipe:0", "-an",
                "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
                "-profile:v", "baseline", "-pix_fmt", "yuv420p", "-bf", "0", "-g", str(fps),
                "-x264-params", "aud=1:repeat-headers=1", "-b:v", "12000000", "-f", "h264",
                "pipe:1" if h264_stream else str(h264_output.resolve())],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE if h264_stream else subprocess.DEVNULL,
                stderr=encoder_log, bufsize=0)
            report["encoding"]["backend"] = "ffmpeg-libx264-software"
            report["encoding"]["hardware"] = False
            encoder_thread = threading.Thread(target=write_frames, daemon=True)
            encoder_thread.start()
            if h264_stream:
                encoded_reader = threading.Thread(target=stream_access_units, daemon=True)
                encoded_reader.start()
        manager = dbus.Interface(bus.get_object(service, "/org/gnome/Mutter/ScreenCast"), service)
        path = manager.CreateSession(dbus.Dictionary({}, signature="sv"), timeout=5)
        session = dbus.Interface(bus.get_object(service, path), service + ".Session")
        # New Mutter versions need an explicit advertised mode; the consumer
        # requests the same geometry. These are D-Bus protocol values, not
        # assumptions inferred from the presence of a Portal service.
        mode = dbus.Dictionary({
            "size": dbus.Struct((dbus.UInt32(width), dbus.UInt32(height))),
            "refresh-rate": dbus.Double(fps),
            "is-preferred": dbus.Boolean(True),
        }, signature="sv")
        stream_path = session.RecordVirtual(dbus.Dictionary({
            "cursor-mode": dbus.UInt32(1),
            "is-platform": dbus.Boolean(True),
            "modes": dbus.Array([mode], signature="a{sv}"),
        }, signature="sv"), timeout=5)
        stream = bus.get_object(service, stream_path)
        matches.append(stream.connect_to_signal("PipeWireStreamAdded", on_node,
                                               dbus_interface=service + ".Stream"))
        matches.append(session.connect_to_signal("Closed", lambda: fail("Mutter session closed")))
        for sig in (signal.SIGINT, signal.SIGTERM):
            signal_handlers[sig] = signal.signal(
                sig, lambda _sig, _frame: GLib.idle_add(fail, "Probe interrupted"))
        session.Start(timeout=5)
        timers.append(GLib.timeout_add(100, check))
        loop.run()
    except Exception as error:
        report.setdefault("errors", []).append(str(error))
    finally:
        for sig, handler in signal_handlers.items():
            signal.signal(sig, handler)
        for match in matches:
            match.remove()
        for timer in timers:
            if GLib.MainContext.default().find_source_by_id(timer):
                GLib.source_remove(timer)
        if pipeline:
            pipeline.set_state(Gst.State.NULL)
            pipeline.get_state(2 * Gst.SECOND)
            pipeline.get_bus().remove_signal_watch()
        if session:
            try:
                session.Stop(timeout=5)
            except Exception as error:
                report.setdefault("errors", []).append("Stop: " + str(error))
        if encoder:
            try:
                encoder_queue.put(None, timeout=2)
                encoder_thread.join(timeout=5)
                if encoder_thread.is_alive():
                    raise TimeoutError("Encoder writer did not stop")
                report["encoding"]["exitCode"] = encoder.wait(timeout=5)
            except (queue.Full, subprocess.TimeoutExpired, TimeoutError) as error:
                encoder_errors.append(str(error))
                encoder.kill()
                encoder.wait(timeout=5)
                # If the writer was waiting for another raw frame, wake it.
                try:
                    encoder_queue.put_nowait(None)
                except queue.Full:
                    pass
                encoder_thread.join(timeout=2)
            encoder_log.seek(0)
            report["encoding"]["stderr"] = encoder_log.read(2000).decode(errors="replace")
            encoder_log.close()
            if encoded_reader:
                encoded_reader.join(timeout=3)
                if encoded_reader.is_alive():
                    encoder_errors.append("Encoded output thread did not stop")
            report["encoding"]["writerStopped"] = not encoder_thread.is_alive()
            if encoder_errors:
                report["encoding"]["errors"] = encoder_errors
        for _ in range(30):
            after = connectors()
            if after == before:
                report["cleanupVerified"] = True
                break
            time.sleep(0.1)
        report["connectorsAfter"] = after
    # A static desktop may only damage a single frame. Frame rate must be
    # certified separately with continuously changing content.
    report["captured"] = report["frames"] >= 1
    report["firstFrameMs"] = round((first_frame - started) * 1000, 2) if first_frame else None
    report["activeBufferArrivalFps"] = round((report["frames"] - 1) / (last_frame - first_frame), 2) \
        if first_frame and last_frame and last_frame > first_frame else 0
    report["passed"] = all(report.get(key) for key in
                           ("created", "extended", "captured", "capturedModeMatches", "cleanupVerified")) \
        and not report.get("errors")
    if h264_output or h264_stream:
        report["encoding"]["output"] = "framed-stdout" if h264_stream else str(h264_output.resolve())
        report["passed"] = report["passed"] and report["encoding"].get("exitCode") == 0 \
            and report["encoding"]["submittedFrames"] > 0 and not encoder_errors \
            and report["encoding"].get("writerStopped", False)
    report["note"] = ("Virtual-output extension, capture, offline software H.264 encoding and cleanup exercised; "
                      "no network session or hardware-encoding certification. File cadence is synthetic, not a performance measurement."
                      if h264_output or h264_stream else
                      "Only virtual-output extension, frame capture and cleanup exercised; not encoding, networking or cross-platform certification. "
                      "Buffer arrival rate on a static desktop is not sustained frame-rate certification.")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exercise-gnome", action="store_true",
                        help="Explicitly create, capture and remove a temporary output in the current user-bus GNOME desktop")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--duration", type=float, default=3)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--h264-output", type=Path,
                        help="Offline software H.264 experiment, only with --exercise-gnome; file must not exist")
    parser.add_argument("--framed-h264-stdout", action="store_true",
                        help="Emit bounded length-prefixed access units on stdout, JSON on stderr; no network listener")
    parser.add_argument("--query-topology", action="store_true", help="Read actual GNOME output identities without creating a display")
    parser.add_argument("--parent-pid", type=int, help="Stop streaming when the owning process exits")
    args = parser.parse_args()
    if args.parent_pid is not None and (args.parent_pid < 1 or not args.framed_h264_stdout):
        parser.error("Parent monitoring requires framed streaming and a positive PID")
    if args.framed_h264_stdout and (not args.exercise_gnome or args.h264_output or args.output):
        parser.error("Framed streaming requires --exercise-gnome and separate diagnostic stderr")
    if args.h264_output and (not args.exercise_gnome or args.h264_output.exists()):
        parser.error("--h264-output requires --exercise-gnome and a new output file")
    if args.h264_output and args.output and args.h264_output.resolve() == args.output.resolve():
        parser.error("JSON and video outputs must be different files")
    if not (16 <= args.width <= 1920 and 16 <= args.height <= 1080 and
            1 <= args.fps <= 60 and (1 <= args.duration <= (3600 if args.framed_h264_stdout else 30) or
                                    (args.duration == 0 and args.framed_h264_stdout and args.parent_pid))):
        parser.error("Mode or duration out of range")
    try:
        if args.query_topology:
            if args.exercise_gnome or args.framed_h264_stdout or args.h264_output:
                parser.error("Topology query must be read-only")
            import dbus
            bus=dbus.SessionBus()
            config=dbus.Interface(bus.get_object("org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig"), "org.gnome.Mutter.DisplayConfig")
            state=config.GetCurrentState(timeout=4)
            report={"schemaVersion":1,"displayIds":sorted(str(monitor[0][0]) for monitor in state[1]),"passed":True}
        else:
            report = exercise_gnome(args.width, args.height, args.fps, args.duration, args.h264_output, args.framed_h264_stdout, args.parent_pid) \
                if args.exercise_gnome else doctor()
    except Exception as error:
        report = {"schemaVersion": 1, "passed": False, "errors": [str(error)],
                  "certified": False}
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    print(text, end="", file=sys.stderr if args.framed_h264_stdout else sys.stdout)
    return 0 if not args.exercise_gnome or report.get("passed") else 1


if __name__ == "__main__":
    sys.exit(main())
