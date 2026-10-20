#!/bin/bash
# Build titan-charge (static aarch64 musl binary). Run on the Mac from
# the project root:  userspace/titan-charge/build.sh   -> out/titan-charge
set -euo pipefail
container exec titan_build_ubuntu bash -c '
	. $HOME/.cargo/env
	cd /workspace/userspace/titan-charge
	cargo build --release --target aarch64-unknown-linux-musl
	cp target/aarch64-unknown-linux-musl/release/titan-charge /workspace/out/titan-charge'
