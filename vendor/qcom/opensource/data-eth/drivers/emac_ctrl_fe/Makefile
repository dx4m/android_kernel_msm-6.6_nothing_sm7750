ccflags-y := -I$(srctree)/include/ -I$(srctree)/include/linux/
ccflags-y += -I$(src)/../include/linux/

ccflags-y += -Wno-error
#CONFIG_EMAC_CTRL_FE=y
obj-m += emac-ctrl-fe-virtio.o
emac-ctrl-fe-virtio-objs := emac_ctrl_fe_virtio.o
