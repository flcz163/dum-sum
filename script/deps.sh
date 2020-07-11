#!/bin/bash

function main()
{
	cd `dirname $0`/../src
	cp `pwd`/arch/arm64/configs/qemu_defconfig .config -f
}

main $*
