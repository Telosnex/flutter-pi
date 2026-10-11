#!/usr/bin/env bash
# Builds the Telosnex flutter-pi runner against the pinned WPE WebKit SDK.
#
#   telosnex/build.sh <output-directory>
#
# Run in a Debian 13 (trixie) arm64 environment after install_build_deps.sh.
# The SDK is downloaded, checked, and extracted at WPE_SDK_PREFIX, the same
# path that tpi installs it to on the Pi. The runner's RUNPATH names that
# path. The CMake flags match the accepted appliance build.
#
# Output: flutter-pi and flutter-pi.json (SHA-256 and build inputs).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="${1:?usage: telosnex/build.sh <output-directory>}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
# shellcheck source=telosnex/wpe-sdk.env
source "$here/wpe-sdk.env"

[ "$(uname -m)" = aarch64 ] || { echo "Build on arm64." >&2; exit 1; }
# shellcheck source=/dev/null
. /etc/os-release
[ "$ID-$VERSION_ID" = debian-13 ] || { echo "Build on Debian 13." >&2; exit 1; }

archive="${WPE_SDK_CACHE:-$root/.wpe-sdk}/wpe-sdk-$WPE_SDK_TARGET-$WPE_SDK_SHA256.tar.zst"
mkdir -p "$(dirname "$archive")"
if [ ! -f "$archive" ] || ! echo "$WPE_SDK_SHA256  $archive" | sha256sum -c --quiet - 2>/dev/null; then
  curl -fsSL --retry 3 -o "$archive.tmp" \
    "https://github.com/Telosnex/flutter_inappwebview/releases/download/$WPE_SDK_TAG/wpe-sdk-$WPE_SDK_TARGET.tar.zst"
  mv "$archive.tmp" "$archive"
fi
echo "$WPE_SDK_SHA256  $archive" | sha256sum -c --quiet -
rm -rf "$WPE_SDK_PREFIX"
mkdir -p "$WPE_SDK_PREFIX"
zstd -dc "$archive" | tar -x -C "$WPE_SDK_PREFIX" --strip-components=1
export PKG_CONFIG_PATH="$WPE_SDK_PREFIX/lib/pkgconfig"

# The OS packages that the SDK loads. Linking against WebKit needs them, and
# the Pi installs the same list.
runtime_packages="$WPE_SDK_PREFIX/share/wpe-sdk/runtime-packages.txt"
[ -s "$runtime_packages" ] || { echo "The SDK has no $runtime_packages." >&2; exit 1; }
missing=()
while IFS= read -r package; do
  dpkg-query -W -f='${db:Status-Status}' "$package" 2>/dev/null | grep -qx installed ||
    missing+=("$package")
done < "$runtime_packages"
if [ "${#missing[@]}" -gt 0 ]; then
  if [ "$(id -u)" = 0 ]; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends "${missing[@]}"
  else
    echo "Install the SDK runtime packages first: ${missing[*]}" >&2
    exit 1
  fi
fi

build="${RUNNER_BUILD_DIR:-$root/build-telosnex}"
rm -rf "$build"
# Session switching OFF: the appliance uses direct DRM/evdev access, and
# upstream turns it ON when libseat-dev happens to be installed.
# TRY_BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN OFF: a missing WPE or GStreamer
# must fail the build, not drop the web view and video plugins.
cmake -S "$root" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_OPENGL=ON -DTRY_ENABLE_OPENGL=OFF \
  -DENABLE_VULKAN=ON -DTRY_ENABLE_VULKAN=OFF -DVULKAN_DEBUG=OFF \
  -DENABLE_SESSION_SWITCHING=OFF \
  -DBUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN=ON -DTRY_BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN=OFF \
  -DBUILD_SENTRY_PLUGIN=OFF -DBUILD_TEST_PLUGIN=OFF \
  -DENABLE_TESTS=ON \
  -DCMAKE_SKIP_RPATH=ON \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath,$WPE_SDK_PREFIX/lib" \
  -DCMAKE_C_FLAGS="-ffile-prefix-map=$root=." \
  -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$root=."
ninja -C "$build"
(cd "$build" && ctest --output-on-failure)

runner="$build/flutter-pi"
webkit="$(ldd "$runner" | awk '$1 == "libWPEWebKit-2.0.so.1" {print $3}')"
[ "$webkit" = "$WPE_SDK_PREFIX/lib/libWPEWebKit-2.0.so.1" ] || {
  echo "flutter-pi loads WebKit from '$webkit', not the SDK." >&2
  exit 1
}
if ldd "$runner" | grep -F 'not found'; then exit 1; fi
# Exactly the SDK. An empty entry would search the working directory.
runpath="$(readelf -d "$runner" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')"
[ "$runpath" = "$WPE_SDK_PREFIX/lib" ] || {
  echo "Unexpected RUNPATH '$runpath'." >&2
  exit 1
}

install -m 0755 "$runner" "$out/flutter-pi"
sha256="$(sha256sum "$out/flutter-pi" | cut -d' ' -f1)"
cat > "$out/flutter-pi.json" <<JSON
{
  "file": "flutter-pi",
  "sha256": "$sha256",
  "revision": "$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)",
  "wpeSdkTag": "$WPE_SDK_TAG",
  "wpeSdkTarget": "$WPE_SDK_TARGET",
  "wpeSdkSha256": "$WPE_SDK_SHA256",
  "wpeSdkPrefix": "$WPE_SDK_PREFIX",
  "compiler": "$(cc --version | head -1)"
}
JSON
cat "$out/flutter-pi.json"
