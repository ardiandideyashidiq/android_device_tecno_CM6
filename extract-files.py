#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: 2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

from extract_utils.fixups_blob import (
    blob_fixup,
    blob_fixups_user_type,
)

from extract_utils.fixups_lib import (
    lib_fixups,
    lib_fixups_user_type,
)

from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)

namespace_imports = [
    'device/tecno/CM6',
    'hardware/mediatek',
    'hardware/mediatek/libmtkperf_client',
    'hardware/mediatek/libaedv',
]

def lib_fixup_vendor_suffix(lib: str, partition: str, *args, **kwargs):
    return f'{lib}_{partition}' if partition == 'vendor' else None

lib_fixups: lib_fixups_user_type = {
    **lib_fixups,
    (
        'libtflite_mtk',
        'libneuron_graph_delegate.mtk',
        'vendor.mediatek.hardware.apuware.apusys@2.0',
        'vendor.mediatek.hardware.apuware.apusys@2.1',
        'vendor.mediatek.hardware.apuware.hmp@1.0',
        'vendor.mediatek.hardware.apuware.utils@2.0',
        'vendor.mediatek.hardware.videotelephony@1.0'
    ): lib_fixup_vendor_suffix,
}

blob_fixups: blob_fixups_user_type = {
    'vendor/lib64/hw/mt6789/android.hardware.camera.provider@2.6-impl-mediatek.so': blob_fixup()
        .replace_needed('libutils.so', 'libutils-v33.so')
        .add_needed('libcamera_metadata_shim.so'),
    'vendor/lib64/mt6789/libmtkcam_stdutils.so': blob_fixup()
        .replace_needed('libutils.so', 'libutils-v33.so'),
    ('vendor/bin/mnld', 'vendor/lib64/mt6789/libcam.utils.sensorprovider.so'): blob_fixup()
        .replace_needed('libsensorndkbridge.so', 'android.hardware.sensors@1.0-convert-shared.so')
        .replace_needed('android.hardware.sensors-V2-ndk.so', 'android.hardware.sensors-V3-ndk.so'),
    'vendor/lib64/hw/audio.primary.mediatek.so': blob_fixup()
        .replace_needed('libtinyxml2.so', 'libtinyxml2-v34.so')
        .replace_needed('android.hardware.audio.effect-V2-ndk.so', 'android.hardware.audio.effect-V3-ndk.so')
        .replace_needed('android.hardware.bluetooth.audio-V4-ndk.so', 'android.hardware.bluetooth.audio-V5-ndk.so')
        .binary_regex_replace(b'A2dpsuspendonly', b'A2dpSuspended\x00\x00')
        .binary_regex_replace(b'BTAudiosuspend', b'A2dpSuspended\x00'),
    'vendor/lib64/hw/android.hardware.audio.effect.aidl-impl-mediatek.so': blob_fixup()
        .patchelf_version('0_17_2')
        .replace_needed('libtinyxml2.so', 'libtinyxml2-v34.so'),
    (
        'vendor/lib64/hw/sensors.mediatek.V2.0.so',
        'vendor/lib64/libcodec2_mtk_c2store.so',
        'vendor/lib64/libcodec2_mtk_vdec.so',
        'vendor/lib64/libcodec2_mtk_venc.so',
        'vendor/lib64/libcodec2_vpp_qt_plugin.so',
        'vendor/lib64/libcodec2_vpp_rs_plugin.so'
    ): blob_fixup()
        .replace_needed('libstagefright_foundation.so', 'libstagefright_foundation-v33.so'),
    'vendor/bin/hw/android.hardware.security.keymint-service.trustonic': blob_fixup()
        .replace_needed('android.hardware.security.keymint-V1-ndk_platform.so', 'android.hardware.security.keymint-V1-ndk.so')
        .replace_needed('android.hardware.security.secureclock-V1-ndk_platform.so', 'android.hardware.security.secureclock-V1-ndk.so')
        .replace_needed('android.hardware.security.sharedsecret-V1-ndk_platform.so', 'android.hardware.security.sharedsecret-V1-ndk.so')
        .add_needed('android.hardware.security.rkp-V3-ndk.so'),
    'vendor/etc/init/android.hardware.neuralnetworks-shim-service-mtk.rc': blob_fixup()
        .regex_replace('start', 'enable'),
    ('vendor/lib64/libspeech_enh_lib.so', 'vendor/lib64/libwifi-hal-mtk.so', 'vendor/lib64/hw/sound_trigger.primary.mt6789.so', 'vendor/lib64/libnir_neon_driver_ndk.mtk.vndk.so'): blob_fixup()
        .fix_soname(),
    'vendor/etc/init/init.thermal_core.rc': blob_fixup()
        .regex_replace('ro.vendor.mtk_thermal_2_0', 'vendor.thermal.link_ready'),
    ('vendor/lib64/mt6789/libneuralnetworks_sl_driver_mtk_prebuilt.so', 'vendor/lib64/libstfactory-vendor.so', 'vendor/lib64/libnvram.so', 'vendor/lib64/libsysenv.so', 'vendor/lib64/libtflite_mtk.so', 'vendor/lib64/nfc_nci_nxp_snxxx.so'): blob_fixup()
        .add_needed('libbase_shim.so'),
    'vendor/lib64/hw/hwcomposer.mtk_common.so': blob_fixup()
        .patchelf_version('0_17_2')
        .add_needed('libprocessgroup_shim.so')
        .replace_needed('libtinyxml2.so', 'libtinyxml2-v34.so')
        .replace_needed('android.hardware.graphics.common-V6-ndk.so', 'android.hardware.graphics.common-V7-ndk.so'),
    ('vendor/lib64/lib3a.ae.pipe.so'): blob_fixup()
        .add_needed('liblog.so'),
    'vendor/lib64/mt6789/libmnl.so': blob_fixup()
        .add_needed('libcutils.so'),
    ('vendor/lib64/mt6789/libneuralnetworks_sl_driver_mtk_prebuilt.so', 'vendor/lib64/mt6789/libeffect_hal.so', 'vendor/lib64/libanc_single_rt_bokeh.so'): blob_fixup()
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_createFromHandle')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_getNativeHandle')
        .clear_symbol_version('AHardwareBuffer_lock')
        .clear_symbol_version('AHardwareBuffer_lockPlanes')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock'),
    'vendor/lib64/mt6789/libtranssion_bodybeauty.so': blob_fixup()
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_createFromHandle')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_getNativeHandle')
        .clear_symbol_version('AHardwareBuffer_lock')
        .clear_symbol_version('AHardwareBuffer_lockPlanes')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock'),
    'vendor/lib64/mt6789/libmorpho_video_stabilizer.so': blob_fixup()
        .add_needed('libutils-v33.so'),
    'vendor/bin/hw/mt6789/camerahalserver': blob_fixup()
        .replace_needed('libutils.so', 'libutils-v33.so'),
    'vendor/bin/hw/mtkfusionrild': blob_fixup()
        .add_needed('libutils-v33.so'),
    'vendor/lib64/librt_extamp_intf.so': blob_fixup()
        .replace_needed('libtinyxml2.so', 'libtinyxml2-v34.so'),
    ('vendor/lib64/libpqxmlparser.so', 'vendor/lib64/libpqxmlflagparser.so', 'vendor/lib64/libsilkybrightnesscore.so'): blob_fixup()
        .patchelf_version('0_17_2')
        .replace_needed('libtinyxml2.so', 'libtinyxml2-v34.so'),

    'vendor/lib64/mt6789/libmtkcam_hal_aidl_common.so': blob_fixup()
        .replace_needed('android.hardware.camera.common-V2-ndk.so', 'android.hardware.camera.common-V1-ndk.so'),
    'vendor/bin/hw/android.hardware.drm-service.clearkey': blob_fixup()
        .replace_needed('android.hardware.drm-V1-ndk.so', 'android.hardware.drm-V2-ndk.so'),
    'vendor/lib64/libgpud.so': blob_fixup()
        .replace_needed('android.hardware.graphics.common-V6-ndk.so', 'android.hardware.graphics.common-V7-ndk.so'),
    'vendor/lib64/hw/mt6789/android.hardware.graphics.allocator-V2-mediatek.so': blob_fixup()
        .replace_needed('android.hardware.graphics.common-V6-ndk.so', 'android.hardware.graphics.common-V7-ndk.so'),
    'vendor/lib64/hw/mt6789/mapper.mediatek.so': blob_fixup()
        .replace_needed('android.hardware.graphics.common-V6-ndk.so', 'android.hardware.graphics.common-V7-ndk.so'),
    'vendor/lib64/hw/mt6789/vendor.mediatek.hardware.pq_aidl-impl.so': blob_fixup()
        .patchelf_version('0_17_2')
        .replace_needed('libtinyxml2.so', 'libtinyxml2-v34.so')
        .replace_needed('android.hardware.sensors-V2-ndk.so', 'android.hardware.sensors-V3-ndk.so'),
    'vendor/bin/hw/mt6789/android.hardware.graphics.allocator-V2-service-mediatek.mt6789': blob_fixup()
        .replace_needed('android.hardware.graphics.common-V6-ndk.so', 'android.hardware.graphics.common-V7-ndk.so'),
    'vendor/lib64/mt6789/libmtkcam_grallocutils.so': blob_fixup()
        .replace_needed('android.hardware.graphics.common-V5-ndk.so', 'android.hardware.graphics.common-V7-ndk.so'),
    'vendor/lib64/libcodec2_fsr.so': blob_fixup()
        .replace_needed('android.hardware.graphics.common-V6-ndk.so', 'android.hardware.graphics.common-V7-ndk.so'),
    'vendor/lib64/vendor.mediatek.hardware.pq_aidl-V8-ndk.so': blob_fixup()
        .replace_needed('android.hardware.graphics.common-V6-ndk.so', 'android.hardware.graphics.common-V7-ndk.so'),
    'vendor/lib64/vendor.mediatek.hardware.camera.isphal-V1-ndk.so': blob_fixup()
        .replace_needed('android.hardware.graphics.common-V6-ndk.so', 'android.hardware.graphics.common-V7-ndk.so'),

    'odm/lib64/libTranssionTone.so': blob_fixup()
        .add_needed('libTranColorEffectEngine.so'),

    'vendor/lib64/mt6789/libneuron_adapter_mgvi.so': blob_fixup()
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_lock')
        .clear_symbol_version('AHardwareBuffer_unlock'),

    'vendor/lib64/libneuralnetworks_sl_driver_mtk_prebuilt.so': blob_fixup()
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_createFromHandle')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_getNativeHandle')
        .clear_symbol_version('AHardwareBuffer_lock')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock'),

    'odm/lib64/libtranssion_bodybeauty.so': blob_fixup()
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_lockPlanes')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock'),

    'vendor/lib64/libtflite_mtk.so': blob_fixup()
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_lock')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock'),

    'odm/lib64/libanc_single_rt_bokeh.so': blob_fixup()
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_lock')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock'),

    'odm/lib64/libeffect_hal.so': blob_fixup()
        .clear_symbol_version('AHardwareBuffer_allocate')
        .clear_symbol_version('AHardwareBuffer_describe')
        .clear_symbol_version('AHardwareBuffer_lockPlanes')
        .clear_symbol_version('AHardwareBuffer_release')
        .clear_symbol_version('AHardwareBuffer_unlock'),

    'vendor/lib64/android.hardware.audio.core-impl-mediatek.so': blob_fixup()
        .add_needed('libaudioutils_shim.so')
        .replace_needed('android.hardware.audio.core-V3-ndk.so', 'android.hardware.audio.core-V4-ndk.so'),

    'vendor/lib64/mt6789/libmtkcam_hwnode.so': blob_fixup()
        .add_needed('libultrahdr_CM6.so'),

    'vendor/bin/hw/android.hardware.audio.service-aidl.mediatek': blob_fixup()
        .replace_needed('android.hardware.audio.core-V3-ndk.so', 'android.hardware.audio.core-V4-ndk.so'),

    'vendor/lib64/libaudioprimarydevicehalifclient.so': blob_fixup()
        .replace_needed('android.hardware.audio.core-V3-ndk.so', 'android.hardware.audio.core-V4-ndk.so'),

    'system_ext/bin/hw/android.hardware.audio.parameter_parser.service': blob_fixup()
        .replace_needed('android.hardware.audio.core-V3-ndk.so', 'android.hardware.audio.core-V4-ndk.so'),

    'system/lib64/av-audio-types-aidl-ndk.so': blob_fixup()
        .replace_needed('android.hardware.audio.core-V3-ndk.so', 'android.hardware.audio.core-V4-ndk.so'),

}  # fmt: skip

module = ExtractUtilsModule(
    'CM6',
    'tecno',
    blob_fixups=blob_fixups,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
    add_firmware_proprietary_file=True,
)

if __name__ == '__main__':
    utils = ExtractUtils.device(module)
    utils.run()
