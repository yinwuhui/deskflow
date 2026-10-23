#!/usr/bin/env python3
"""Generate only the macOS application icon (requires resvg-py and iconutil).

The sidebar, tray, Windows and Linux branding are intentionally independent.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

import resvg_py

ROOT = Path(__file__).resolve().parents[1]
BRAND = ROOT / "src/apps/res/brand"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=BRAND / "deskflow-logo-macos.svg")
    args = parser.parse_args()
    svg = args.source.read_text(encoding="utf-8")
    (BRAND / "deskflow-logo-macos.svg").write_text(svg, encoding="utf-8")

    def render(size):
        return resvg_py.svg_to_bytes(svg_string=svg, width=size, height=size, skip_system_fonts=True)

    # Qt's runtime Dock override uses the same artwork as the bundle's ICNS.
    (BRAND / "deskflow-macos-1024.png").write_bytes(render(1024))
    with tempfile.TemporaryDirectory(prefix="bmouse-macos-icons-") as temp:
        iconset = Path(temp) / "Deskflow.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                suffix = "@2x" if scale == 2 else ""
                (iconset / f"icon_{size}x{size}{suffix}.png").write_bytes(render(size * scale))
        subprocess.run(["/usr/bin/iconutil", "-c", "icns", str(iconset), "-o",
                        str(ROOT / "src/apps/res/Deskflow.icns")], check=True)
    print("Updated macOS application icon only.")


if __name__ == "__main__":
    main()
