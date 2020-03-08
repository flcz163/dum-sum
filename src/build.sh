#!/bin/bash

#make ARCH=arm64 qemu_defconfig
make ARCH=arm64 EXTRA_CFLAGS="-g -D__LINUX_ARM_ARCH__=8 -DCONFIG_QEMU=1" CROSS_COMPILE=`pwd`/../toolchains/gcc-linaro-5.3-2016.02-x86_64_aarch64-elf/bin/aarch64-elf- Image dtbs

