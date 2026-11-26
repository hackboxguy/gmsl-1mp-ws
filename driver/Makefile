# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build of the Waveshare GMSL-1MP-Camera-A driver.
#
#   make                      native build against the running kernel
#   make KERNEL_SRC=<dir> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

obj-m := gmsl1mp.o
gmsl1mp-y := gmsl1mp_ser.o gmsl1mp_v4l2.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) clean

.PHONY: all modules_install clean
