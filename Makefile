KERN_ROOT ?= $(abspath ../CactKernel-x86_32)
LOCAL_REPO ?= $(abspath ../LocalRepoCactOS)

_ACTIVE := $(filter-out clean,$(or $(MAKECMDGOALS),all))
ifneq ($(_ACTIVE),)
ifndef KERN_ROOT
$(error KERN_ROOT is required — path to kernel sources with Cact/ headers)
endif
ifndef LOCAL_REPO
$(error LOCAL_REPO is required — directory whose lib/ receives *.cctk)
endif
endif
INSTALL_DIR := $(LOCAL_REPO)/lib

MOD_CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib \
	-I$(KERN_ROOT)/Cact/kernel/net \
	-I$(KERN_ROOT)/Cact/kernel/sync \
	-I$(KERN_ROOT)/Cact/drivers/pci/enum \
	-I$(KERN_ROOT)/Cact/drivers/pci \
	-I. \
	-Wall -O2

.PHONY: all install clean
all: virtio_net.cctk

virtio_net.cctk: virtio_net_mod.o
	cp -f $< $@

virtio_net_mod.o: virtio_net_mod.c virtio_net.h
	gcc $(MOD_CFLAGS) -c virtio_net_mod.c -o $@

install: virtio_net.cctk
	@mkdir -p $(INSTALL_DIR)
	cp -f virtio_net.cctk $(INSTALL_DIR)/virtio_net.cctk
	@echo "installed: $(INSTALL_DIR)/virtio_net.cctk"

clean:
	rm -f virtio_net_mod.o virtio_net.cctk
