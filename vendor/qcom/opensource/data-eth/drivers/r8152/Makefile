ifeq ($(KP_MODULE_ROOT),)
	KP_MODULE_ROOT=$(KERNEL_SRC)/$(M)
endif

ifeq ($(RELEASE_PACKAGE),1)
EXTRA_CFLAGS+=-DRELEASE_PACKAGE
endif
LBITS := $(shell getconf LONG_BIT)
ifeq ($(LBITS),64)
CCFLAGS += -m64
EXTRA_CFLAGS+=-DSYSTEM_IS_64
else
CCFLAGS += -m32
endif

M ?= $(shell pwd)

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

obj-m := r8152_rtl.o

KBUILD_OPTIONS+=KBUILD_DTC_INCLUDE=$(KP_MODULE_ROOT)
KBUILD_OPTIONS+=KBUILD_DUPLICATED_EXPORTS_NO_WARN=1

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) M=$(M) -C $(KERNEL_SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)
