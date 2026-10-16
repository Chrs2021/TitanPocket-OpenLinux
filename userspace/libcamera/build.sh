#!/bin/bash
# Build and install libcamera for the Titan Pocket cameras (docs/camera.md).
#
# Runs on the phone (or in an arm64 Debian 12 chroot): fetches libcamera
# v0.7.2, applies 0001-titan-pocket.patch and installs into /usr/local with
# the simple pipeline handler, the software ISP (GPU debayer through
# Panfrost), the cam tool, the GStreamer element and the V4L2 compatibility
# layer. Needs kernel #32 or later (udmabuf, V4L2 buffer orphaning) and the
# titan_camera module.
#
#   userspace/libcamera/build.sh [SRCDIR]     (default /root/src/libcamera)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${1:-/root/src/libcamera}
TAG=v0.7.2

DEBIAN_FRONTEND=noninteractive apt-get install -y -q \
	git meson ninja-build pkg-config g++ python3-yaml python3-ply python3-jinja2 \
	libyaml-dev libudev-dev libevent-dev libgnutls28-dev openssl \
	libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
	libjpeg-dev libtiff-dev libdrm-dev libegl-dev libgles-dev libgbm-dev

if [ ! -d "$SRC" ]; then
	git clone https://git.libcamera.org/libcamera/libcamera.git "$SRC"
fi
cd "$SRC"
git fetch -q --tags
git checkout -q -f "$TAG"
git apply "$HERE/0001-titan-pocket.patch"

rm -rf build
meson setup build --prefix=/usr/local --buildtype=release \
	-Dpipelines=simple -Dipas=simple -Dsoftisp-gpu=enabled \
	-Dgstreamer=enabled -Dcam=enabled -Dqcam=disabled -Dv4l2=enabled \
	-Dpycamera=disabled -Ddocumentation=disabled -Dtest=false \
	-Dlc-compliance=disabled -Dtracing=disabled
ninja -C build
ninja -C build install
ldconfig
