# Makefile for OpenWRT system

INCLUDE_DIR += ${TOPDIR}/src/data-eth/drivers/thermal-eth-netlink

CFLAGS +=-I${TOPDIR}/src/data-eth/drivers/thermal-eth-netlink \
	-Wundef \
	-Wstrict-prototypes \
	-Wno-trigraphs \
	-Werror \
	-g -O0 \
	-fno-inline \
	-fno-short-enums \
	-fpic \
	-Wextra \
	-Wno-strict-aliasing \
	-Wno-unused-function \
	-Wno-unused-variable \
	-Wno-stringop-truncation \
	-Wno-unused-parameter \
	-Wno-format-truncation \
	-Wno-missing-field-initializers \
	-Wno-switch \
	-D_GNU_SOURCE \
	-DENABLE_THERMAL_NETLINK \
	-I../inc \
	-I${STAGING_DIR}/linux-msm/usr/include \
	-I${STAGING_DIR}/usr/include/libnl3

RM := rm -rf

THERMAL-ETH-NETLINK := thermal-eth-netlink

all: $(THERMAL-ETH-NETLINK)

LIBPATH := thermal-eth-netlink
LDFLAGS += -L$(LIBPATH)

THERMAL-ETH-NETLINK_SRCS = thermal.c \
			thermal_util_netlink.c

THERMAL-ETH-NETLINK_OBJS := $(THERMAL-ETH-NETLINK_SRCS:.c=.o)

$(THERMAL-ETH-NETLINK): $(THERMAL-ETH-NETLINK_OBJS)
	$(CC) $(THERMAL-ETH-NETLINK_OBJS) $(CFLAGS) $(LDFLAGS) $(DSUTILS_CFLAGS) $(DSUTILS_LIBS) -lc -lrt -lpthread -ldsutils -lnl -lnl-genl -o $@
	mkdir -p bin
	mv thermal-eth-netlink bin/

clean:
	$(RM) bin/
	$(RM) *.o
