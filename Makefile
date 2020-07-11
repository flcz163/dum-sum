
CWD = $(shell pwd)

all: kernel run

devel:
	sh ./script/devel.sh

deps:
	sh ./script/deps.sh

.PHONY: deps

kernel:
	cd src; make ARCH=arm64 EXTRA_CFLAGS="-g -D__LINUX_ARM_ARCH__=8 -DCONFIG_QEMU=1" CROSS_COMPILE=`pwd`/../toolchains/gcc-linaro-5.3-2016.02-x86_64_aarch64-elf/bin/aarch64-elf- Image dtbs

run:
	cd src; sudo -S qemu-system-aarch64 -machine virt -cpu cortex-a53 -smp 4 -m 1024 -serial stdio -kernel arch/arm64/boot/Image -drive file=./dim-sum.img,if=none,id=blk -device virtio-blk-device,drive=blk -device virtio-net-device,netdev=network0,mac=52:54:00:4a:1e:d4 -netdev tap,id=network0,ifname=tap0 --append "earlyprintk console=ttyAMA0 root=/dev/vda rootfstype=ext3 init=/bin/ash rw ip=10.0.0.10::10.0.0.1:255.255.255.0:::off"

clean:
	cd src; make clean

distclean:
	cd src; make distclean
