# SPDX-License-Identifier: GPL-2.0
vendor := $(srctree)/$(src)

ifeq ($(CONFIG_NOTHING_IS_METROID), y)
	ifneq "$(wildcard $(vendor)/qcom)" ""
		subdir-y += qcom
	endif
else ifeq ($(CONFIG_NOTHING_IS_FROGGERPRO), y)
	ifneq "$(wildcard $(vendor)/qcom-froggerpro)" ""
		subdir-y += qcom-froggerpro
	endif
endif