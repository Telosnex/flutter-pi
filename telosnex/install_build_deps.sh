#!/usr/bin/env bash
# Installs the packages that telosnex/build.sh needs, as root, on Debian 13
# (trixie) arm64. The WPE WebKit SDK supplies WebKit; its own runtime
# packages come from the SDK's runtime-packages.txt.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  ca-certificates cmake curl file gcc g++ git ninja-build pkg-config zstd \
  libdrm-dev libegl-dev libgbm-dev libgles-dev libglib2.0-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libinput-dev \
  libsoup-3.0-dev libsystemd-dev libudev-dev libvulkan-dev libxkbcommon-dev \
  nlohmann-json3-dev
