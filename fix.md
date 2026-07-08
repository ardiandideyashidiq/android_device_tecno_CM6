# Build fixes for LineageOS 23.2 bp4a on tecno/CM6

## Context

- Device: tecno/CM6 (MediaTek MT6789)
- Build target: `lineage_CM6-bp4a-eng` (64-bit only)
- Dump: `/home/ashallowperson/android_development/CM6/Dump/out`
- Device tree: `device/tecno/CM6`
- Vendor tree: `vendor/tecno/CM6` (never modified)

All fixes are in `proprietary-files.txt`, `extract-files.py`, and
`sepolicy/vendor/property_contexts` in the device tree.

---

## 1. SELinux property namespace violations

### Error
```
init: : Unable to set property "ro.mtk_cam_dualzoom_support" to "1":
  SELinux property service denial
init: Could not set property 'ro.mtk_cam_dualzoom_support': SELinux
```

The build also aborts in Soong with a failing check
(`SelinuxCheckPropertyService`).

### Cause
In bp4a builds, properties without an explicit `ro.vendor.` or `vendor.`
prefix trigger SELinux denials. Several entries in
`sepolicy/vendor/property_contexts` used bare `ro.mtk_*` or
`RUNTIME_OVERRIDE_*` prefixes, and `RUNTIME_OVERRIDE_*` used
`exported_default_prop` (system context) instead of a vendor context.

### Fix (`sepolicy/vendor/property_contexts`)

```diff
-ro.mtk_cam_dualzoom_support                 u:object_r:vendor_mtk_camera_prop:s0
-ro.mtk_cam_stereo_camera_support            u:object_r:vendor_mtk_camera_prop:s0
+ro.vendor.mtk_cam_stereo_camera_support     u:object_r:vendor_mtk_camera_prop:s0

-RUNTIME_OVERRIDE_OPENCL_MEM_TYPE            u:object_r:exported_default_prop:s0
-RUNTIME_OVERRIDE_LOG_LEVEL                  u:object_r:exported_default_prop:s0
+vendor.RUNTIME_OVERRIDE_OPENCL_MEM_TYPE     u:object_r:vendor_mtk_default_prop:s0
+vendor.RUNTIME_OVERRIDE_LOG_LEVEL           u:object_r:vendor_mtk_default_prop:s0

-ro.mtk_key_manager_support                  u:object_r:vendor_mtk_default_prop:s0
+ro.vendor.mtk_key_manager_support           u:object_r:vendor_mtk_default_prop:s0
```

Also removed a duplicate `ro.vendor.mtk_cam_dualzoom_support` entry (already
present in `device/mediatek/sepolicy_vndr/base/vendor/property_contexts`).

---

## 2. SONAME mismatches (DT_SONAME != filename)

### Error
```
error: DT_SONAME "libalsautils.so" must be equal to the file name "libalsautils-v32.so".
error: DT_SONAME "libspeech_enh.so" must be equal to the file name "libspeech_enh_lib.so".
error: DT_SONAME "libcrypto.so" must be equal to the file name "libtrancrypto.so".
```

### Cause
Prebuilt blobs have an internal DT_SONAME that differs from their filename.
In bp4a builds the check is stricter and rejects the mismatch.

### Fix (`extract-files.py`)

Added `.fix_soname()` for the following blobs (patches the ELF DT_SONAME to
match the filename):

```python
'vendor/lib/libalsautils-v32.so': blob_fixup()
    .fix_soname(),
'vendor/lib64/libalsautils-v32.so': blob_fixup()
    .fix_soname(),
```

Extended the existing tuple:

```python
('vendor/lib64/libspeech_enh_lib.so', 'vendor/lib/libspeech_enh_lib.so',
 'vendor/lib64/libtrancrypto.so', 'vendor/lib/libtrancrypto.so', ...)
    .fix_soname(),
```

---

## 3. Unresolved symbols from libbase (32-bit nvram, neuralnetworks, etc.)

### Error
```
vendor/lib64/libstfactory-vendor.so: Unresolved symbol: Trim@LIBBASE
vendor/lib64/libnvram.so: Unresolved symbol: Basename@LIBBASE
vendor/lib64/mt6789/libneuralnetworks_sl_driver_mtk_prebuilt.so:
  Unresolved symbol: AHardwareBuffer_allocate@LIBNATIVEWINDOW
```

### Cause
Prebuilt blobs reference functions from `libbase` (e.g., `android::base::Trim`,
`android::base::Basename`) and `libnativewindow` (`AHardwareBuffer_*`) but the
VNDK linker cannot resolve the versioned symbols in bp4a.

### Fix (`extract-files.py`)

For `libbase` symbols, added a shim library `libbase_shim.so`:

```python
('vendor/lib64/mt6789/libneuralnetworks_sl_driver_mtk_prebuilt.so',
 'vendor/lib64/libstfactory-vendor.so', 'vendor/lib64/libnvram.so',
 'vendor/lib/libsysenv.so', 'vendor/lib64/libsysenv.so',
 'vendor/lib64/libtflite_mtk.so'):
    blob_fixup().add_needed('libbase_shim.so'),
```

For `libnativewindow` symbols, stripped the version info from symbol
references:

```python
('vendor/lib64/mt6789/libneuralnetworks_sl_driver_mtk_prebuilt.so',
 'vendor/lib64/mt6789/libeffect_hal.so', 'vendor/lib64/libMegviiHum.so',
 'vendor/lib64/libanc_single_rt_bokeh.so',
 'vendor/lib64/libvideofilmeffect.so'):
    blob_fixup()
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_createFromHandle')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_getNativeHandle')
        .clear_symbol_version('AHardwareBuffer_lock')
        .clear_symbol_version('AHardwareBuffer_lockPlanes')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock'),
```

---

## 4. Missing libutils dependency (libmorpho_video_stabilizer.so)

### Error
```
vendor/lib64/mt6789/libmorpho_video_stabilizer.so:
  Unresolved symbol: _ZN7android14sp_report_raceEv
  Unresolved symbol: _ZNK7android7RefBase9decStrongEPKv
  Unresolved symbol: _ZNK7android7RefBase9incStrongEPKv
```

### Cause
The blob uses `android::RefBase` and `android::sp_report_race` from `libutils`
but does not declare `libutils.so` in its DT_NEEDED. The VNDK linker does not
resolve transitive dependencies through `libui.so` (which happens to be linked).

### Fix (`extract-files.py`)

```python
'vendor/lib64/mt6789/libmorpho_video_stabilizer.so': blob_fixup()
    .add_needed('libutils.so'),
```

---

## 5. 32-bit blob removals (cascade from missing bp4a 32-bit deps)

### Error
```
error: dependency "libnvram" of "libfile_op" missing variant
  os:android,image:vendor,arch:arm_armv8-2a,link:shared
```

### Cause
Several 32-bit (`vendor/lib/`) blobs had no variant available in bp4a because
their dependencies (`libnvram`, `libfile_op`, etc.) could not be built for
32-bit ARM. The removal cascaded through the dependency chain.

### Fix (`proprietary-files.txt`)

Removed the following 32-bit blobs (all have 64-bit counterparts in
`vendor/lib64/`):

| Removed blob | Reason |
|---|---|
| `vendor/lib/libnvram.so` | Unresolved `libbase` symbols |
| `vendor/lib/mt6789/libneuralnetworks_sl_driver_mtk_prebuilt.so` | Unresolved `libnativewindow` symbols |
| `vendor/lib/mt6789/libaalservice.so` | Unresolved symbols |
| `vendor/lib/libfile_op.so` | Cascade from `libnvram` removal |
| `vendor/lib/libcs_cs35l45_intf.so` | Cascade from `libfile_op` removal |
| `vendor/lib/libnxp_extamp_intf.so` | Cascade from `libfile_op` removal |
| `vendor/lib/librt_extamp_intf.so` | Cascade from `libfile_op` removal |
| `vendor/lib/libaudiocustparam_vendor.so` | Cascade from `libnvram` removal |
| `vendor/lib/libaudiocompensationfilterc.so` | Cascade from `libaudiocustparam_vendor` removal |
| `vendor/lib/libaudioloudc.so` | Cascade from `libaudiocompensationfilterc` removal |
| `vendor/lib/libaudiosmartpamtk.so` | Cascade from `libnvram` removal |
| `vendor/lib/libbluetooth_mtk.so` | Cascade from `libnvram` removal |
| `vendor/lib/libbluetooth_mtk_pure.so` | Cascade from `libnvram` removal |

All were removed by commenting them out with explanations in
`proprietary-files.txt`.

---

## Outcome

```
$ m vendorimage
[100% 3101/3101]
#### build completed successfully ####

$ ls -lh out/target/product/CM6/vendor.img
-rw-r--r-- 1006M vendor.img
```
