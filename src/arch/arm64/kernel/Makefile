#
# Makefile for the linux kernel.
#

CPPFLAGS_vmlinux.lds := -DTEXT_OFFSET=$(TEXT_OFFSET)
AFLAGS_head.o        := -DTEXT_OFFSET=$(TEXT_OFFSET)

# Object file lists.

obj-y := exception.o processor.o psci.o setup.o cpu.o irq.o smp.o \
	alternative.o configs.o stack.o traps.o

head-y			:= head.o
extra-y := $(head-y) vmlinux.lds
