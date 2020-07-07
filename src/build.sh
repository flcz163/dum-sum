#!/bin/bash

tool_chains_dir=`pwd`/../toolchains
tool_chains_gcc_dir=gcc-linaro-5.3-2016.02-x86_64_aarch64-elf
tool_chains_name=gcc-linaro-5.3-2016.02-x86_64_aarch64-elf.tar.bz2
config_dir=`pwd`/arch/arm64/configs/qemu_defconfig
config_name=.config

function first_complie(){
    if [ -f $config_name ];then
        return
    fi

    cp $config_dir $config_name
}

function first_toolchain(){
    if [ -d $tool_chains_dir/$tool_chains_gcc_dir ];then
        return
    fi

    cd $tool_chains_dir

    echo "merge tool chain file..."
    cat xa* > $tool_chains_name

    echo "Unpack the tool chain package..."
    tar xf $tool_chains_name

    cd -
}

first_complie

first_toolchain

make ARCH=arm64 EXTRA_CFLAGS="-g -D__LINUX_ARM_ARCH__=8 -DCONFIG_QEMU=1" CROSS_COMPILE=$tool_chains_dir/$tool_chains_gcc_dir/bin/aarch64-elf- Image dtbs

