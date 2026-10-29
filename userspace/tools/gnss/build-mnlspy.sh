#!/bin/bash
# Build libmnlspy.so (32-bit ARM, for bionic): see mnlspy.c. Output in out/gnss-re/.
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p out/gnss-re
container exec titan_build_ubuntu bash -c '
	cd /workspace/tools/gnss
	clang-11 --target=armv7a-linux-androideabi -O2 -fPIC -shared -nostdlib \
		-fuse-ld=lld-11 -Wl,--hash-style=both -Wl,-soname,libmnlspy.so \
		-o /workspace/out/gnss-re/libmnlspy.so mnlspy.c
	file /workspace/out/gnss-re/libmnlspy.so'
