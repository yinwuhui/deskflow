#!/usr/bin/env python3
"""Build a locally certificate-signed preview without invalidating TCC on each update."""

import argparse
import plistlib
import shutil
import subprocess
import tempfile
from pathlib import Path


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--qt-bin", required=True, type=Path)
    parser.add_argument("--identity", required=True, help="Apple Development signing identity or SHA-1")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--output-dir", type=Path, default=root / "dist/home-refined")
    args = parser.parse_args()
    if args.identity.strip() == "-":
        parser.error("A certificate is required; ad-hoc signing breaks permission continuity.")

    output = args.output_dir.resolve()
    destination = output / "busymouse.app"
    source = args.build_dir.resolve() / "bin/busymouse.app"
    if destination == source:
        parser.error("The preview output must be separate from the build bundle.")
    # A running process must exit before its executable and signature are replaced.
    processes = subprocess.check_output(["/bin/ps", "-axo", "comm="], text=True).splitlines()
    executable = str(destination / "Contents/MacOS/busymouse")
    if executable in (line.strip() for line in processes):
        parser.error("Quit the running preview normally before updating this bundle.")

    run(args.cmake, "--build", args.build_dir, "--target", "busymouse", "busymouse-core", "-j", "6")
    translation = args.build_dir / "translations/busymouse_zh_CN.qm"
    translation.parent.mkdir(parents=True, exist_ok=True)
    run(args.qt_bin / "lrelease", root / "translations/busymouse_zh_CN.ts", "-qm", translation)
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".preview-stage-", dir=output) as stage:
        app = Path(stage) / destination.name
        shutil.copytree(source, app, symlinks=True)
        translations = app / "Contents/Resources/translations"
        translations.mkdir(parents=True, exist_ok=True)
        shutil.copy2(translation, translations / translation.name)
        info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
        if info["CFBundleIdentifier"] != "org.busymouse.busymouse":
            parser.error("Keep the established preview Bundle ID to preserve its permission identity.")
        # Deployment rewrites Mach-O load paths; certificate signing must be the final step.
        run(args.qt_bin / "macdeployqt", app,
            "-executable=" + str(app / "Contents/MacOS/busymouse-core"), "-always-overwrite", "-no-strip")
        # All dependencies are bundled and signed together; no development-only entitlements are needed.
        run("/usr/bin/codesign", "--force", "--deep", "--options", "runtime", "--timestamp=none",
            "--sign", args.identity, app)
        run("/usr/bin/codesign", "--verify", "--deep", "--strict", app)
        signed = subprocess.run(["/usr/bin/codesign", "-d", "-r-", str(app)],
                                text=True, capture_output=True, check=True)
        requirement = signed.stdout + signed.stderr
        if "designated => cdhash" in requirement or "anchor apple" not in requirement:
            raise RuntimeError("The preview must have a stable certificate-backed designated requirement.")
        if destination.exists():
            shutil.rmtree(destination)
        shutil.move(str(app), destination)

    archive = output.parent / "BMouse-Home-Preview-macos-arm64.zip"
    run("/usr/bin/ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", destination, archive)
    run("/usr/bin/unzip", "-tq", archive)
    (output / "README.zh-CN.md").write_text(
        "# BMouse 本机开发预览\n\n"
        "请始终运行此目录下的 busymouse.app，并使用同一个开发证书生成更新。\n"
        "从旧的临时签名首次切换到证书签名后，需要在系统设置中重新确认一次辅助功能授权。\n"
        "后续更新由同一证书、同一 Bundle ID 标识，不再以每次构建的哈希作为身份。\n"
        "此包使用本机开发证书，供当前开发环境验证；正式发行应使用发行签名和公证。\n",
        encoding="utf-8",
    )
    print(destination)
    print(archive)


if __name__ == "__main__":
    main()
