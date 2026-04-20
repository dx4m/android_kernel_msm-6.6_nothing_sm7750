# SPDX-License-Identifier: GPL-2.0-only
ifeq ($(KP_MODULE_ROOT),)
	KP_MODULE_ROOT=$(KERNEL_SRC)/$(M)
endif

M ?= $(shell pwd)

r8168-y := r8168_n.o \
		rtl_eeprom.o \
		rtltool.o \
		r8168_rss.o \
		r8168_asf.o \
		r8168_lib.o \
		r8168_firmware.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

KBUILD_OPTIONS+=KBUILD_DTC_INCLUDE=$(KP_MODULE_ROOT)
KBUILD_OPTIONS+=CONFIG_R8168=m

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean