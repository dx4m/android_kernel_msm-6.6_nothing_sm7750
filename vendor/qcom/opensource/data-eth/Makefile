# SPDX-License-Identifier: GPL-2.0-only

ccflags-y := -Wno-unused-function
obj-y := data-eth.o

ifneq ($(CONFIG_ETHQOS_QCOM_SVM), y)
obj-m += drivers/ioss/
obj-m += drivers/emac_ioss/
endif

ifeq ($(CONFIG_ARCH_SDXPINN), y)
obj-m += drivers/r8125/src/
obj-m += drivers/r8125_ioss/
obj-m += drivers/r8168/src/
obj-m += drivers/r8168_ioss/
obj-m += drivers/aqc_ioss/
endif

obj-$(CONFIG_QPS615) += drivers/qps615/src/
obj-$(CONFIG_QPS615_IOSS) += drivers/qps615_ioss/
obj-$(CONFIG_QTI_QUIN_GVM) += drivers/emac_ctrl_fe/
obj-$(CONFIG_EMAC_SHIM) += drivers/emac_shim/

ifeq ($(KP_MODULE_ROOT),)
	KP_MODULE_ROOT=$(KERNEL_SRC)/$(M)
endif

M ?= $(shell pwd)

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

KBUILD_OPTIONS+=KBUILD_DTC_INCLUDE=$(KP_MODULE_ROOT)

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) M=$(M) -C $(KERNEL_SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean

%:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $@ $(KBUILD_OPTIONS)
