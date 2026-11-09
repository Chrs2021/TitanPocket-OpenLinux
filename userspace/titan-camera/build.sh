#!/bin/bash
# Build titan-camera against the phone's Debian 12 libraries (the rootfs build tree in
# the container, as a sysroot), so it links GTK 4.8 and glibc 2.36. Tests run inside that
# tree. From the project root:  userspace/titan-camera/build.sh  -> out/titan-camera
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cat > "$ROOT/out/.titan-camera-build.sh" <<'IN'
set -euo pipefail
. $HOME/.cargo/env
C=/tmp/debian-rootfs
which pkg-config >/dev/null || apt-get install -y -qq pkg-config >/dev/null
export PKG_CONFIG_SYSROOT_DIR=$C
export PKG_CONFIG_LIBDIR=$C/usr/lib/aarch64-linux-gnu/pkgconfig:$C/usr/share/pkgconfig:$C/usr/lib/pkgconfig
export PKG_CONFIG_ALLOW_CROSS=1
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=gcc
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_RUSTFLAGS="-C link-arg=--sysroot=$C -C link-arg=-B$C/usr/lib/aarch64-linux-gnu/ -C link-arg=-L$C/usr/lib/aarch64-linux-gnu -C link-arg=-L$C/lib/aarch64-linux-gnu -C link-arg=-Wl,--allow-shlib-undefined -C link-arg=-Wl,-rpath-link,$C/usr/lib/aarch64-linux-gnu:$C/lib/aarch64-linux-gnu"
cd /workspace/userspace/titan-camera
T=aarch64-unknown-linux-gnu
cargo test --release --target $T --no-run 2>&1 | tail -40
BIN=$(ls -t target/$T/release/deps/titan_camera-* | grep -v '\.d$' | sed -n 1p)
cp "$BIN" $C/tmp/titan-camera-test
chroot $C /tmp/titan-camera-test 2>&1 | tail -14
cargo build --release --target $T 2>&1 | tail -40
cp target/$T/release/titan-camera /workspace/out/titan-camera
ls -la /workspace/out/titan-camera
# linked in the VM, so let Debian's own loader check that everything resolves
cp /workspace/out/titan-camera $C/tmp/titan-camera-check
chroot $C /bin/sh -c 'ldd -r /tmp/titan-camera-check 2>&1 | grep -E "not found|undefined symbol" || echo "all symbols resolve"'
chroot $C /bin/sh -c 'cp /dev/null /dev/null' 2>/dev/null || true
IN
container exec titan_build_ubuntu bash /workspace/out/.titan-camera-build.sh
