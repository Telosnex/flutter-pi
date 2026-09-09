#!/usr/bin/env bash
# Host-only tests; no libinput, DRM, Flutter engine, or running Pi needed.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
if [[ ! -f "$root/third_party/Unity/src/unity.c" ]]; then
  echo 'Run: git submodule update --init third_party/Unity' >&2
  exit 1
fi
build="$(mktemp -d "${TMPDIR:-/tmp}/flutter-pi-scroll-tests.XXXXXX")"
trap 'rm -rf "$build"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -DUNITY_SUPPORT_64 -DUNITY_INCLUDE_DOUBLE \
  -I"$root/src" -I"$root/third_party/flutter_embedder_header/include" \
  -I"$root/third_party/Unity/src" \
  "$root/test/user_input_scroll_test.c" "$root/src/user_input_scroll.c" \
  "$root/third_party/Unity/src/unity.c" -lm -o "$build/user_input_scroll_test"
"$build/user_input_scroll_test"
