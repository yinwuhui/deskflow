#!/usr/bin/env bash
# Read-only inventory for the macOS/Linux acceptance session.
set -u
if [ "$(uname -s)" != Linux ]; then
  echo 'Run this inventory on the Linux desktop used for testing.' >&2
  exit 2
fi
printf '%s\n' '== OS and desktop session =='
if [ -r /etc/os-release ]; then cat /etc/os-release; fi
uname -m
printf 'XDG_SESSION_TYPE=%s\nXDG_CURRENT_DESKTOP=%s\n' "${XDG_SESSION_TYPE:-unknown}" "${XDG_CURRENT_DESKTOP:-unknown}"
printf '%s\n' '== Build tools =='
for tool in cmake ninja c++; do
  if command -v "$tool" >/dev/null 2>&1; then "$tool" --version 2>&1 | head -n 1; else printf '%s: unavailable\n' "$tool"; fi
done
if command -v openssl >/dev/null 2>&1; then openssl version; else echo 'openssl: unavailable'; fi
printf '%s\n' '== Development libraries (missing headers do not prove runtime is absent) =='
if command -v pkg-config >/dev/null 2>&1; then
  for module in Qt6Core Qt6Widgets Qt6Network Qt6DBus openssl libei-1.0 libportal xkbcommon xkbregistry x11 xtst xrandr xi xinerama xkbfile; do
    if pkg-config --exists "$module"; then printf '%s: %s\n' "$module" "$(pkg-config --modversion "$module")"; else printf '%s: unavailable in pkg-config\n' "$module"; fi
  done
else
  echo 'pkg-config: unavailable'
fi
printf '%s\n' '== Portal interfaces (introspection only; no authorization requested) =='
if command -v gdbus >/dev/null 2>&1; then
  gdbus introspect --session --dest org.freedesktop.portal.Desktop --object-path /org/freedesktop/portal/desktop 2>&1 | sed -n '/interface org.freedesktop.portal.\(RemoteDesktop\|InputCapture\|Clipboard\|GlobalShortcuts\)/,/^  };/p'
else
  echo 'gdbus unavailable; Portal capabilities not inspected'
fi
