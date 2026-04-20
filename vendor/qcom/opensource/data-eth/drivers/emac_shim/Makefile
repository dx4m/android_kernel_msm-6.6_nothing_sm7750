ccflags-y := -I$(srctree)/include/ -I$(srctree)/include/linux/
ccflags-y += -I$(srctree)/include/net/
ccflags-y += -I$(src)/../include/linux/

ccflags-y += -Wno-error
obj-m += stmmac.o
stmmac-objs:= stmmac_thin.o stmmac_ethtool_thin.o \
	      dwmac4_descs.o dwmac4_thin.o hwif_thin.o \
	      stmmac_platform_thin.o dwmac-qcom-ethqos-thin.o dwmac-qcom-msgq-svm.o \
	      dwxgmac2_descs.o dwxgmac2_thin.o

#ifeq ($(CONFIG_ETHQOS_QCOM_SVM), y)
#stmmac-objs+= dwmac-qcom-msgq-svm.o
#endif
