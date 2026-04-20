#dtbo-$(CONFIG_ARCH_SUN)   := sun-camera.dtbo
#dtbo-$(CONFIG_ARCH_SUN)   += sun-camera-sensor-mtp.dtbo \
#				sun-camera-sensor-rumi.dtbo \
#				sun-camera-sensor-cdp.dtbo  \
#				sun-camera-sensor-hdk.dtbo  \
#				sun-camera-sensor-qrd.dtbo

#dtbo-$(CONFIG_ARCH_TUNA)  += tuna-camera.dtbo

#dtbo-$(CONFIG_ARCH_TUNA)  += tuna-camera-sensor-mtp.dtbo \
#				tuna-camera-sensor-cdp.dtbo \
#				tuna-camera-sensor-qrd.dtbo

dtbo-$(CONFIG_ARCH_SUN)   := arbok-camera.dtbo
dtbo-$(CONFIG_ARCH_SUN)   += arbok-camera-sensor-t0.dtbo

ifeq ($(CONFIG_NOTHING_IS_FROGGERPRO), y)
dtbo-$(CONFIG_ARCH_KERA)  += froggerpro-camera.dtbo
dtbo-$(CONFIG_ARCH_KERA)  += froggerpro-camera-sensor-qrd.dtbo
endif

# dtbo-$(CONFIG_ARCH_KERA)  += kera-camera.dtbo
# dtbo-$(CONFIG_ARCH_KERA)  += kera-camera-sensor-mtp.dtbo \
# 				kera-camera-sensor-cdp.dtbo \
# 				kera-camera-sensor-qrd.dtbo \
# 				kera-camera-sensor-rcm.dtbo
