#!/bin/sh
# Native Linux build. Configure flutter-pi first (for config.h).
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
build=${1:-"$root/build"}
pkgs='wpe-webkit-2.0 wpe-platform-2.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-gl-egl-1.0 gbm egl glesv2 libdrm xkbcommon'
# pkg-config output is intentionally split into compiler arguments.
${CC:-cc} -O2 -g -std=gnu17 -DNDEBUG -DWPE_ENABLE_XKB=1 \
    -I"$build" -I"$root/src" -I"$root/third_party/flutter_embedder_header/include" \
    $(pkg-config --cflags $pkgs) "$root/test/webview/harness.c" \
    "$root/src/plugins/gstreamer_video_player/webview_direct.c" \
    "$root/src/plugins/gstreamer_video_player/webview_gpu.c" \
    -o "$build/webview-harness" $(pkg-config --libs $pkgs) -lpthread -lm
