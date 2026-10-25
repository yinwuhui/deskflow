#!/bin/sh
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output_dir=${1:-/tmp/deskflow-macos-display-probe}
contents="$output_dir/VirtualDisplayProbe.app/Contents"
mkdir -p "$contents/MacOS"
cat > "$contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>org.deskflow.VirtualDisplayProbe</string>
<key>CFBundleExecutable</key><string>macos-display-probe</string>
<key>CFBundleName</key><string>VirtualDisplayProbe</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>0.1</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
xcrun clang++ -std=c++20 -fobjc-arc -fblocks -Wall -Wextra -Werror \
  -Wno-unused-parameter -mmacosx-version-min=14.0 \
  -framework Foundation -framework AppKit -framework CoreGraphics \
  -framework CoreMedia -framework CoreVideo -framework ScreenCaptureKit -framework VideoToolbox \
  "$source_dir/macos/VirtualDisplayProbe.mm" -o "$contents/MacOS/macos-display-probe"
codesign --force --sign - "$output_dir/VirtualDisplayProbe.app"
printf '%s\n' "$contents/MacOS/macos-display-probe"
