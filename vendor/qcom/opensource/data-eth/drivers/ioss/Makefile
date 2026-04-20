ifeq ($(KP_MODULE_ROOT),)
	KP_MODULE_ROOT=$(KERNEL_SRC)/$(M)
endif

M ?= $(shell pwd)

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

ioss-y := \
	ioss_main.o \
	ioss_bus.o \
	ioss_pci.o \
	ioss_net.o \
	ioss_of.o \
	ioss_ipa.o \
	ioss_ipa_hal.o \
	ioss_mem.o \
	ioss_utils.o \
	ioss_platform.o \
	ioss_qos.o \
	ioss_sysfs.o

KBUILD_OPTIONS+=KBUILD_DTC_INCLUDE=$(KP_MODULE_ROOT)
KBUILD_OPTIONS+=CONFIG_IOSS=m

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) M=$(M) -C $(KERNEL_SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)
