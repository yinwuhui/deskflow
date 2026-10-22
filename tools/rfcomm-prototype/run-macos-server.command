#!/bin/zsh
# SPDX-License-Identifier: MIT
set -eu
prototype_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
prototype_binary="$prototype_dir/busymouse-rfcomm.app/Contents/MacOS/busymouse-rfcomm"
if [[ ! -x "$prototype_binary" ]]; then
  print '请将此启动器与 busymouse-rfcomm.app 放在同一目录。'
  read -r '?按回车退出。'
  exit 1
fi
print 'busymouse RFCOMM Mac 服务端原型（命令行）'
print '请输入允许连接的 Linux 蓝牙地址，例如 AA:BB:CC:DD:EE:FF：'
read -r prototype_peer
exec "$prototype_binary" --server --peer "$prototype_peer"
