#!/usr/bin/env python3
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Manual media experiment. No display capture, pairing or product certification."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prefix = args.sdk_prefix.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="loopback-", dir=args.output))
    pc_dirs = [p for p in (prefix / "lib/pkgconfig", *prefix.glob("lib/*/pkgconfig")) if p.is_dir()]
    if not pc_dirs:
        parser.error("SDK prefix has no pkg-config directory")
    lib_dirs = [p.parent for p in pc_dirs]
    plugin_dirs = [p / "gstreamer-1.0" for p in lib_dirs if (p / "gstreamer-1.0").is_dir()]
    scanner = next(prefix.rglob("gst-plugin-scanner"), None)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("GST_", "DYLD_", "LD_"))}
    env["PKG_CONFIG_PATH"] = os.pathsep.join(map(str, pc_dirs))
    env["GST_PLUGIN_SYSTEM_PATH_1_0"] = os.pathsep.join(map(str, plugin_dirs))
    env["GST_PLUGIN_PATH_1_0"] = ""
    env["GST_REGISTRY"] = str(run / "registry.bin")
    env["GST_DEBUG"] = "1"
    env["GST_DEBUG_NO_COLOR"] = "1"
    env["DYLD_LIBRARY_PATH" if sys.platform == "darwin" else "LD_LIBRARY_PATH"] = os.pathsep.join(map(str, lib_dirs))
    if scanner:
        env["GST_PLUGIN_SCANNER"] = str(scanner)
    pkg = shutil.which("pkg-config")
    compiler = shutil.which("cc")
    if not pkg or not compiler:
        parser.error("cc and pkg-config are required")
    flags = shlex.split(subprocess.check_output(
        [pkg, "--cflags", "--libs", "gstreamer-1.0", "gstreamer-webrtc-1.0"], env=env, text=True))
    source = Path(__file__).with_name("gstreamer_loopback.c")
    executable = run / "gstreamer_loopback"
    subprocess.run([compiler, "-DGST_USE_UNSTABLE_API", str(source), "-o", str(executable), *flags], env=env, check=True)
    log = run / "media.log"
    timed_out = False
    with log.open("w") as output:
        try:
            process = subprocess.run([str(executable)], env=env, stdout=output, stderr=subprocess.STDOUT, timeout=25)
            exit_code = process.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            exit_code = -1
    text = log.read_text(errors="replace")
    metrics = {key: int(value) for key, value in re.findall(r"\b([a-z][a-z0-9_]*)=(\d+)\b", text)}
    caps = re.search(r"^decoded_output_caps=(.*)$", text, re.MULTILINE)
    report = {
        "timestampUtc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "sdkPrefix": str(prefix),
        "gstreamerVersion": subprocess.check_output([pkg, "--modversion", "gstreamer-1.0"], env=env, text=True).strip(),
        "sourceSha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "exitCode": exit_code,
        "timedOut": timed_out,
        "metrics": metrics,
        "decodedOutputCaps": caps.group(1) if caps else None,
        "scope": "Synthetic loopback only; not authenticated product IPC, virtual-output, presentation or hardware certification",
    }
    (run / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"directory": str(run), **report}, ensure_ascii=False, indent=2))
    return int(exit_code != 0)


if __name__ == "__main__":
    raise SystemExit(main())
