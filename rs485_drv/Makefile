obj-m += rs485_dir.o

KDIR ?= /home/lubancat/lubancat_4.19_sdk/kernel
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
MODULE_DIR := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(MODULE_DIR) \
		modules

clean:
	$(MAKE) -C $(KDIR) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(MODULE_DIR) \
		clean



