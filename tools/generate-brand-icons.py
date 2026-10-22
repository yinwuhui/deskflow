#!/usr/bin/env python3
"""Generate app branding from one SVG. Developer dependencies: resvg-py, Pillow.

Run from any directory:
  python tools/generate-brand-icons.py --source output/logo/deskflow-logo-v1.svg

Only resource generation requires these Python packages; the application uses
embedded PNGs and has no added runtime dependency on an SVG plugin.
"""
import argparse
import io
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from PIL import Image
import resvg_py

ROOT = Path(__file__).resolve().parents[1]
BRAND = ROOT / "src/apps/res/brand"


def render(svg, size):
    return resvg_py.svg_to_bytes(svg_string=svg, width=size, height=size, skip_system_fonts=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=BRAND / "deskflow-logo.svg")
    args = parser.parse_args()
    svg = args.source.read_text(encoding="utf-8")
    BRAND.mkdir(parents=True, exist_ok=True)
    (BRAND / "deskflow-logo.svg").write_text(svg, encoding="utf-8")
    resources = ["brand/deskflow-logo.svg"]
    for size in (32, 64, 128, 256, 512, 1024):
        name = f"deskflow-{size}.png"
        (BRAND / name).write_bytes(render(svg, size))
        resources.append("brand/" + name)
    for variant, color in (("dark", "#000000"), ("light", "#ffffff")):
        # Change fill only; preserve all user-authored geometry and transparency.
        symbolic = re.sub(r'fill="#[0-9a-fA-F]{6}"', f'fill="{color}"', svg)
        name = f"deskflow-symbolic-{variant}.png"
        (BRAND / name).write_bytes(render(symbolic, 256))
        resources.append("brand/" + name)
        theme = "light" if variant == "dark" else "dark"
        theme_dir = ROOT / f"src/apps/res/icons/deskflow-{theme}/apps/64"
        (theme_dir / "org.deskflow.deskflow.svg").write_text(svg, encoding="utf-8")
        (theme_dir / "org.deskflow.deskflow-symbolic.svg").write_text(symbolic, encoding="utf-8")
    large = Image.open(io.BytesIO(render(svg, 1024)))
    large.save(ROOT / "src/apps/res/deskflow.ico", format="ICO", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    (ROOT / "deploy/linux/org.deskflow.deskflow.png").write_bytes(render(svg, 512))
    if sys.platform == "darwin":
        with tempfile.TemporaryDirectory(prefix="deskflow-icons-") as temp:
            iconset = Path(temp) / "Deskflow.iconset"
            iconset.mkdir()
            for size in (16, 32, 128, 256, 512):
                for scale in (1, 2):
                    suffix = "@2x" if scale == 2 else ""
                    (iconset / f"icon_{size}x{size}{suffix}.png").write_bytes(render(svg, size * scale))
            subprocess.run(["/usr/bin/iconutil", "-c", "icns", str(iconset), "-o", str(ROOT / "src/apps/res/Deskflow.icns")], check=True)
    else:
        large.save(ROOT / "src/apps/res/Deskflow.icns", format="ICNS")
    qrc = ROOT / "src/apps/res/deskflow.qrc"
    text = re.sub(r'^\s*<file>brand/[^<]+</file>\n', '', qrc.read_text(), flags=re.MULTILINE)
    entries = ''.join(f'        <file>{name}</file>\n' for name in resources)
    text = text.replace('    <qresource prefix="/">\n', '    <qresource prefix="/">\n' + entries, 1)
    qrc.write_text(text)
    print("Updated branding PNG, SVG, ICO, ICNS and Linux application icon.")


if __name__ == "__main__":
    main()
