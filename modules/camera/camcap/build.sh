#!/bin/bash
# Build titan_camcap.ko against our kernel tree (in the build container).
set -euo pipefail
container exec titan_build_ubuntu bash -c '
	make -C /workspace/kernel_vol_mnt/kernel_workspace M=/workspace/modules/camera/camcap -j8 modules 2>&1 | grep -E "error|warning|LD|CC" || true
	ls -l /workspace/modules/camera/camcap/titan_camcap.ko'
