#
# Copyright (C) 2023 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

$(call inherit-product, hardware/lineage/compat/frameworks/compat.mk)

# Inherit from device makefile.
$(call inherit-product, device/tecno/CM6/device.mk)

# Inherit some common LineageOS stuff.
$(call inherit-product, vendor/infinity/config/common_full_phone.mk)

BOARD_VENDOR := TECNO
PRODUCT_NAME := infinity_CM6
PRODUCT_DEVICE := CM6
PRODUCT_MANUFACTURER := TECNO
PRODUCT_BRAND := TECNO
PRODUCT_MODEL := TECNO Camon 40 Pro 4G

PRODUCT_GMS_CLIENTID_BASE := android-transsion

PRODUCT_BUILD_PROP_OVERRIDES += \
    DeviceName=CM6 \
    BuildFingerprint=TECNO/CM6-OP/TECNO-CM6:16/BP2A.250605.031.A3/201500012:user/release-keys

PRODUCT_PRODUCT_PROPERTIES += ro.product.name=CM6-OP

# Maintainer Name
INFINITY_MAINTAINER := "qiratdahaf"

# Whether the device supports Fingerprint On Display
TARGET_HAS_UDFPS := true

# Whether Including Google Apps
WITH_GAPPS := true

PERF_ANIM_OVERRIDE := true
