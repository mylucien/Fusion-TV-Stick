# SPDX-License-Identifier: GPL-2.0
#
# dvb-usb-smit -- SMIT iCast DTMB/DVBC USB dongle driver
#
# Port target: airoha/an7581 (ARMv8 Cortex-A53), kernel 6.18.52
#
# The DVB stack is compiled out-of-tree from the exact kernel version, so the
# target kernel does NOT need CONFIG_MEDIA_SUPPORT / CONFIG_DVB_CORE enabled.
#
# Required environment:
#   KERNEL_DIR    prepared kernel tree of the target (make modules_prepare done)
#   ARCH          arm64
#   CROSS_COMPILE aarch64 toolchain prefix, e.g. .../bin/aarch64-openwrt-linux-
#
# Options:
#   NO_RC=1 (default) dvb-usb built without rc-core / input-core dependency
#   NO_RC=0           also build rc-core.ko (needs CONFIG_INPUT + CONFIG_LEDS_TRIGGERS)
#
# Output: dvb-core.ko  dvb-usb.ko  dvb-usb-smit.ko  [rc-core.ko]

SRC_DIR := $(CURDIR)/src
KSRC    := $(CURDIR)/kern
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm64
NO_RC ?= 1

ccflags-y += -DCONFIG_DVB_NET
ccflags-y += -I$(KSRC)/dvb-core
ccflags-y += -I$(KSRC)/dvb-usb
ccflags-y += -I$(KSRC)/rc
ccflags-y += -I$(SRC_DIR)

# ---- dvb-core -------------------------------------------------------------
# 6.18 object list. Note: dvb_math.o was dropped upstream, do not re-add it.
# dvb_vb2.o is intentionally omitted (CONFIG_DVB_MMAP is off; dvb_vb2.h
# provides static inline stubs for that case).
obj-m += dvb-core.o
dvb-core-objs := \
	$(KSRC)/dvb-core/dvbdev.o \
	$(KSRC)/dvb-core/dmxdev.o \
	$(KSRC)/dvb-core/dvb_demux.o \
	$(KSRC)/dvb-core/dvb_ca_en50221.o \
	$(KSRC)/dvb-core/dvb_frontend.o \
	$(KSRC)/dvb-core/dvb_net.o \
	$(KSRC)/dvb-core/dvb_ringbuffer.o

# ---- dvb-usb --------------------------------------------------------------
obj-m += dvb-usb.o
dvb-usb-objs := \
	$(KSRC)/dvb-usb/dvb-usb-init.o \
	$(KSRC)/dvb-usb/dvb-usb-urb.o \
	$(KSRC)/dvb-usb/dvb-usb-i2c.o \
	$(KSRC)/dvb-usb/dvb-usb-dvb.o \
	$(KSRC)/dvb-usb/dvb-usb-firmware.o \
	$(KSRC)/dvb-usb/usb-urb.o

ifeq ($(NO_RC),1)
dvb-usb-objs += $(SRC_DIR)/dvb-usb-remote-stub.o
else
obj-m += rc-core.o
rc-core-objs := $(KSRC)/rc/rc-main.o $(KSRC)/rc/rc-ir-raw.o
dvb-usb-objs += $(KSRC)/dvb-usb/dvb-usb-remote.o
endif

# ---- the driver itself ----------------------------------------------------
# smit.c (protocol + bridge) and icast-fe.c (frontend) are one module, so the
# frontend is called directly instead of through dvb_attach()/symbol_request().
obj-m += dvb-usb-smit.o
dvb-usb-smit-objs := $(SRC_DIR)/smit.o $(SRC_DIR)/icast-fe.o $(SRC_DIR)/icast-smit.o

# ---- build ----------------------------------------------------------------
all:
	@test -d "$(KERNEL_DIR)" || { echo "KERNEL_DIR not set/exists: $(KERNEL_DIR)"; exit 1; }
	@test -d "$(KSRC)/dvb-core" || { echo "run scripts/prepare-src.sh first"; exit 1; }
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) NO_RC=$(NO_RC) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -f *.ko *.o *.mod *.mod.c *.symvers .*.cmd modules.order Module.symvers

distclean: clean
	rm -rf kern

help:
	@echo "make KERNEL_DIR=<tree> [NO_RC=1] [ARCH=arm64] [CROSS_COMPILE=...]"

.PHONY: all clean distclean help
