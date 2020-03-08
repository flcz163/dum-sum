lib-y      += rbtree.o dump_stack.o scatterlist.o virt_space.o object.o
obj-y      += string.o ctype.o vsprintf.o find_next_bit.o radix-tree.o

ifneq ($(CONFIG_HAVE_DEC_LOCK),y)
  lib-y += dec_and_lock.o
endif

obj-y	+= ioremap.o
obj-y	+= idr.o
CFLAGS_ioremap.o = -O0 
