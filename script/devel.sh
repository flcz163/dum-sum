#!/bin/bash

main()
{
	cd `dirname $0`/../toolchains

	echo "merge tool chain file..."
	cat xa* > gcc-linaro-5.3-2016.02-x86_64_aarch64-elf.tar.bz2

	echo "Unpack the tool chain package..."
	tar xf gcc-linaro-5.3-2016.02-x86_64_aarch64-elf.tar.bz2
}

main $*
