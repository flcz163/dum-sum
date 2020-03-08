LINUXINCLUDE := $(KLIBCINCLUDE)

subdir-ccflags-y := -Iusr/include -Iinclude/kapi/ -Iadapter/klibc/include/ -Iadapter/klibc/include/arch/arm \
	-include include/dim-sum/cmd.h -include include/dim-sum/usr_app_entry.h

subdir-ccflags-y += -I$(srctree)/include/uapi -I$(srctree)/arch/arm64/include/uapi -I$(srctree)/arch/arm64/include/generated -I$(srctree)/arch/arm64/include/generated/uapi 
ifeq ($(CONFIG_64BIT),y)
subdir-ccflags-y += -I$(srctree)/adapter/klibc/include/bits64
else
subdir-ccflags-y += -I$(srctree)/adapter/klibc/include/bits32
endif

subdir-ccflags-y += -D__KLIBC__ -D_BITSIZE=64

obj-y	= app.o lwip_file.o
obj-y	+= shell/
