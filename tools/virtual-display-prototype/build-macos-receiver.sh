#!/bin/sh
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output_dir=${1:-/tmp/deskflow-macos-display-receiver}
contents="$output_dir/DisplayReceiverProbe.app/Contents"
mkdir -p "$contents/MacOS"
cat > "$contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>org.deskflow.NativeDisplayReceiverProbe</string>
<key>CFBundleExecutable</key><string>macos-display-receiver</string>
<key>CFBundleName</key><string>NativeDisplayReceiverProbe</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
xcrun clang++ -std=c++20 -fobjc-arc -fblocks -Wall -Wextra -Werror \
  -Wno-unused-parameter -mmacosx-version-min=14.0 \
  -framework AppKit -framework QuartzCore \
  "$source_dir/macos/DisplayReceiverProbe.mm" -o "$contents/MacOS/macos-display-receiver"
codesign --force --sign - "$output_dir/DisplayReceiverProbe.app"
printf '%s\n' "$contents/MacOS/macos-display-receiver"
