BINARY=b6a189a0-7697-4aa8-9d62-80f64ec4e74d

TARGET_CORE ?= A76
TA_INSTALL_DIR ?= /lib/optee_armtz

ifeq ($(origin TA_DEV_KIT_DIR), undefined)
ifeq ($(TARGET_CORE),A55)
TA_DEV_KIT_DIR := /home/fangjiang/workspace/optee-os/out/arm-plat-mediatek/export-ta_arm64
else ifeq ($(TARGET_CORE),A78)
TA_DEV_KIT_DIR := /home/fangjiang/workspace/optee-os/out/arm-plat-mediatek/export-ta_arm64
else ifeq ($(TARGET_CORE),A76)
TA_DEV_KIT_DIR := /home/jf/workspace/optee-os/out/arm-plat-rpi3/export-ta_arm64
else
$(error Unknown TARGET_CORE '$(TARGET_CORE)'. Use A55, A78, or A76)
endif
endif

include $(TA_DEV_KIT_DIR)/mk/ta_dev_kit.mk

.PHONY: install deploy
install deploy: all
# 	sudo mkdir -p $(TA_INSTALL_DIR)
	sudo cp $(BINARY).ta $(TA_INSTALL_DIR)/
