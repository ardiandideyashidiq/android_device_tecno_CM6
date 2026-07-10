#!/usr/bin/env bash
set -euo pipefail

IMG_DIR="/home/ashallowperson/android_development/lineageos_23.2/out/target/product/CM6"

fastboot flash vendor      "$IMG_DIR/vendor.img"
fastboot flash boot        "$IMG_DIR/boot.img"
fastboot flash vendor_boot "$IMG_DIR/vendor_boot.img"
fastboot flash vendor_dlkm "$IMG_DIR/vendor_dlkm.img"
fastboot flash odm_dlkm    "$IMG_DIR/odm_dlkm.img"
fastboot flash product     "$IMG_DIR/product.img"
fastboot flash system      "$IMG_DIR/system.img"
fastboot flash system_ext  "$IMG_DIR/system_ext.img"

echo "All partitions flashed successfully."