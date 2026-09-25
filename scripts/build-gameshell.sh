#!/bin/sh
# ClockworkPi GameShell (Allwinner R16: 4x Cortex-A7, ARMv7 hard-float) host
# tools, cross-built on this machine with Zig as static musl binaries that run
# on any GameShell image: pspexp-term, pspexp-bench, pspexp-sprite-demo.
#   scripts/build-gameshell.sh    ->  build/gameshell/bin/
exec env ZIG_TARGET=arm-linux-musleabihf ZIG_CPU=cortex_a7 CROSS_NAME=gameshell \
	CROSS_HOST=arm-linux-musleabihf CROSS_PROC=arm "$(dirname "$0")/build-omega.sh" "$@"
