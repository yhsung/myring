# Kernel module Kbuild Makefile
obj-m := myring.o
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Cross-compilation support
CROSS_COMPILE ?= 
CC := $(CROSS_COMPILE)gcc

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Cross-compile user application (macOS host)
user-cross:
	aarch64-elf-gcc -static -O2 -Wall -o user-aarch64 user.c

user:
	$(CC) -O2 -Wall -o user user.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f user user-aarch64
