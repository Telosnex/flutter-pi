#!/bin/sh
# Run on the Pi, separately from the app. Preserve all failures, including exit
# crashes/timeouts. Private D-Bus avoids SSH accessibility activation stalls.
set -eu
binary=${1:?usage: run.sh /path/to/webview-harness [cycles]}
cycles=${2:-6}
command -v timeout >/dev/null
command -v dbus-run-session >/dev/null
cache=$(mktemp -d)
trap 'rm -rf "$cache"' EXIT HUP INT TERM
export XDG_CACHE_HOME="$cache/cache" XDG_DATA_HOME="$cache/data"
export FLUTTERPI_WEBVIEW_STATS=1
export HARNESS_VIDEO_FIXTURE="${HARNESS_VIDEO_FIXTURE:-$(dirname "$0")/video.html}"
# 240 s is an upper bound, not an expected runtime. Timeout also bounds D-Bus.
timeout --kill-after=5s "${HARNESS_TIMEOUT:-240}s" dbus-run-session -- "$binary" "$cycles"
