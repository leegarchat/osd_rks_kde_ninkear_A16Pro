#!/usr/bin/env bash
# build.sh — сборка полностью статического rsk_osd
set -e
cd "$(dirname "$0")"

WL_A=/tmp/opencode/wl/build/src/libwayland-client.a
FFI_A=/usr/lib/x86_64-linux-gnu/libffi.a

[ -f "$WL_A" ] || { echo "нет $WL_A: сначала собери wayland static"; exit 1; }
[ -f wlr-layer-shell-unstable-v1.c ] || \
  wayland-scanner private-code wlr-layer-shell-unstable-v1.xml wlr-layer-shell-unstable-v1.c
[ -f wlr-layer-shell-unstable-v1.h ] || \
  wayland-scanner client-header wlr-layer-shell-unstable-v1.xml wlr-layer-shell-unstable-v1.h
[ -f font_bold.h ] || \
  xxd -i -n rsk_font_bold /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf font_bold.h

gcc -O2 -static -o rsk_osd rsk_osd.c wlr-layer-shell-unstable-v1.c \
  -I. "$WL_A" "$FFI_A" -lrt -lpthread -ldl -lm

echo "---"
ls -la rsk_osd
file rsk_osd
