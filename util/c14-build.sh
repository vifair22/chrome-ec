#!/bin/bash
# Build the ThinkPad C14 Chromebook's EC RW image (primus) in the coreboot
# SDK container: what coreboot embeds as ec.RW.flat and EC software sync
# writes to the EC.
#
#   util/c14-build.sh [BOARD]     default primus; output build/BOARD/RW/ec.RW.flat
#
# The container, toolchain and flags are those MrChromebox's chrome-ec-build
# (build-docker.sh, its "sdk-arm" path) uses for the brya boards: the SDK's
# /opt/xgcc arm-eabi gcc, host tools from the container's gcc, and upstream's
# warning set with -Wno-error, since the SDK's gcc is newer than the one the
# release branch was tuned for. A warning is still a failure here: the build
# log is checked and the script fails if any compiler warning was printed.
set -euo pipefail

BOARD=${1:-primus}
IMAGE=${EC_IMAGE:-coreboot/coreboot-sdk:2024-12-21_306660c2de}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LOG="$ROOT/build/c14-build-$BOARD.log"

mkdir -p "$ROOT/build"
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/src:rw" -w /src "$IMAGE" \
	bash -lc '
	set -euo pipefail
	export PATH=/opt/xgcc/bin:$PATH CCACHE_DISABLE=1
	cross=(COREBOOT_SDK_ROOT_arm=/opt/xgcc CROSS_COMPILE=/opt/xgcc/bin/arm-eabi-)
	warn="-Wall -Wundef -Wno-error -Werror-implicit-function-declaration -Wno-trigraphs -Wno-format-security -Wno-address-of-packed-member -fno-common -fno-strict-aliasing -fno-strict-overflow"
	make BOARD='"$BOARD"' "${cross[@]}" clean
	make -j"$(nproc)" BOARD='"$BOARD"' BUILDCC=gcc BUILDCC_PREFIX= "${cross[@]}" \
		PEM= "COMMON_WARN=$warn" \
		build/'"$BOARD"'/RW/ec.RW.elf build/'"$BOARD"'/RW/ec.RW.smap
	/opt/xgcc/bin/arm-eabi-objcopy --set-section-flags .roshared=share -O binary \
		build/'"$BOARD"'/RW/ec.RW.elf build/'"$BOARD"'/RW/ec.RW.flat
	' 2>&1 | tee "$LOG"

if grep -q "warning:" "$LOG"; then
	echo "c14-build: compiler warnings in $LOG" >&2
	exit 1
fi
out="$ROOT/build/$BOARD/RW/ec.RW.flat"
sha256sum "$out"
grep -a -o -E "${BOARD}_v[0-9.]+-[0-9a-f]+[^[:cntrl:]]{0,60}" "$out" | head -1
