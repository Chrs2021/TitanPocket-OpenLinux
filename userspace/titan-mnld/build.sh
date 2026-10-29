#!/bin/bash
# Build titan-mnld: a 32-bit bionic executable linked against the stock
# libmnl.so and bionic libraries staged in out/android-gps/root (never
# redistributed; see tools/gnss/stage-mnld.sh). Output: out/titan-mnld.
set -euo pipefail
cd "$(dirname "$0")/../.."
container exec titan_build_ubuntu bash -c '
	set -e
	cd /workspace/userspace/titan-mnld
	R=/workspace/out/android-gps/root
	CFLAGS="--target=armv7a-linux-androideabi -mcpu=cortex-a53 -mthumb -O2 -fPIE \
		-fno-stack-protector -ffreestanding -fno-builtin -Wall -Wextra -Wno-unused-parameter"
	clang-11 $CFLAGS -c titan-mnld.c -o /tmp/titan-mnld.o
	clang-11 $CFLAGS -c start.c -o /tmp/start.o
	ld.lld-11 -pie --dynamic-linker=/system/bin/linker --hash-style=both \
		--export-dynamic -z noexecstack -z relro -z now \
		-o /workspace/out/titan-mnld /tmp/start.o /tmp/titan-mnld.o \
		$R/vendor/lib/libmnl.so $R/system/lib/libc.so $R/system/lib/libdl.so
	file /workspace/out/titan-mnld
	llvm-readelf-11 -d /workspace/out/titan-mnld | grep NEEDED'
