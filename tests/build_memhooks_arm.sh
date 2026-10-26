#!/bin/sh
# Builds and runs test_memhooks_arm: the memory-call stand-ins (plugin/Sources/mem_hooks.s) on the
# 3DS's CPU type under qemu-arm, with a fake system call in place of `svc`.
# (qemu runs the default CPU: the cross toolchain's start-up code is ARMv7; the stand-ins are
# still assembled for ARMv6K, so they use nothing the 3DS lacks.)
#   apt install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf qemu-user
set -e
cd "$(dirname "$0")"
ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp -marm"
arm-linux-gnueabihf-g++ $ARCH -static -O2 -std=gnu++17 -fno-exceptions -fno-rtti -I../plugin/Includes \
  -Wa,--defsym,MEMHOOK_TEST=1 \
  -o test_memhooks_arm test_memhooks_arm.cpp ../plugin/Sources/core/mem_share.cpp \
  ../plugin/Sources/mem_hooks.s memhooks_harness.s
qemu-arm ./test_memhooks_arm
