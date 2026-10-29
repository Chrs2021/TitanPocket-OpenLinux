#!/bin/bash
# Build titan-gps (static aarch64 musl binaries). Run on the Mac from the
# project root:  userspace/titan-gps/build.sh   -> out/titan-gps-probe
set -euo pipefail
container exec titan_build_ubuntu bash -c '
	. $HOME/.cargo/env
	cd /workspace/userspace/titan-gps
	cargo test --release
	cargo build --release --target aarch64-unknown-linux-musl
	cp target/aarch64-unknown-linux-musl/release/titan-gps-probe /workspace/out/titan-gps-probe'
