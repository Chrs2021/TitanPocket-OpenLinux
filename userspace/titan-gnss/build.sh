#!/bin/bash
# Build titan-gnss (static aarch64 musl binary). Run on the Mac from
# the project root:  userspace/titan-gnss/build.sh   -> out/titan-gnss
set -euo pipefail
container exec titan_build_ubuntu bash -c '
	. $HOME/.cargo/env
	cd /workspace/userspace/titan-gnss
	cargo build --release --target aarch64-unknown-linux-musl
	cp target/aarch64-unknown-linux-musl/release/titan-gnss /workspace/out/titan-gnss'
