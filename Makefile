# Kernel module Kbuild Makefile
obj-m := myring.o
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
BUILD_DIR := build

# Cross-compilation support
CROSS_COMPILE ?= 
CC := $(CROSS_COMPILE)gcc

all: $(BUILD_DIR)
	# Copy source files to build directory
	cp myring.c myring_uapi.h $(BUILD_DIR)/
	# Also copy Makefile for kbuild
	echo "obj-m := myring.o" > $(BUILD_DIR)/Makefile
	$(MAKE) -C $(KDIR) M=$(PWD)/$(BUILD_DIR) modules
	# Copy final kernel module to build root
	cp $(BUILD_DIR)/myring.ko $(BUILD_DIR)/ 2>/dev/null || true

# Cross-compile user application (macOS host)
user-cross: $(BUILD_DIR)
	aarch64-elf-gcc -static -O2 -Wall -o $(BUILD_DIR)/user-aarch64 user.c

user: $(BUILD_DIR)
	$(CC) -O2 -o $(BUILD_DIR)/user user.c

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
	# Clean any stray artifacts in source directory
	rm -f *.ko *.o *.mod *.mod.c *.mod.o *.symvers *.order
	rm -f .*.cmd .*.d
	rm -rf .tmp_versions/

.PHONY: all user user-cross clean
