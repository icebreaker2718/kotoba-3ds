#!/bin/sh
# Builds tests/ocr_cli for the 3DS's CPU (ARM11, ARMv6K + VFP, ARM mode, the plugin's flags) with
# a Linux ARM cross compiler, so qemu-arm can run the ARMv6 SIMD kernel.
#   apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf qemu-user
set -e
cd "$(dirname "$0")"
ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp -marm -fomit-frame-pointer"
mkdir -p arm_build
for f in ../plugin/Sources/vocr/*.c; do
  arm-linux-gnueabihf-gcc $ARCH -O2 -std=gnu99 -ffp-contract=off -I../plugin/Includes/vocr -c "$f" -o "arm_build/$(basename "$f" .c).o"
done
arm-linux-gnueabihf-g++ $ARCH -static -Os -std=gnu++17 -fno-exceptions -fno-rtti -I../plugin/Includes -I../plugin/Includes/vocr \
  -o ocr_cli_arm ocr_cli.cpp ../plugin/Sources/core/local_ocr.cpp ../plugin/Sources/core/textbox.cpp \
  ../plugin/Sources/core/bmp.cpp ../plugin/Sources/core/textlines.cpp ../plugin/Sources/core/kana.cpp \
  ../plugin/Sources/core/correct.cpp ../plugin/Sources/core/dict.cpp ../plugin/Sources/core/search.cpp \
  ../plugin/Sources/core/deinflect.cpp arm_build/*.o -lm
arm-linux-gnueabihf-objdump -d arm_build/vocr.o | grep -q smlad && echo "ocr_cli_arm built (ARMv6 SIMD kernel in use)"
