diff --git a/configs/power/powerhint.json b/configs/power/powerhint.json
index 0dc4e98..b764a76 100644
--- a/configs/power/powerhint.json
+++ b/configs/power/powerhint.json
@@ -396,25 +396,25 @@
     {
       "PowerHint": "INTERACTION",
       "Node": "CPUEfficiencyClusterMinFreq",
-      "Duration": 1500,
-      "Value": "1350000"
+      "Duration": 3000,
+      "Value": "1500000"
     },
     {
       "PowerHint": "INTERACTION",
       "Node": "CPUSuperClusterMinFreq",
-      "Duration": 1500,
-      "Value": "1400000"
+      "Duration": 3000,
+      "Value": "1700000"
     },
     {
       "PowerHint": "INTERACTION",
       "Node": "CPUEfficiencyClusterMaxFreq",
-      "Duration": 1500,
+      "Duration": 3000,
       "Value": "1600000"
     },
     {
       "PowerHint": "INTERACTION",
       "Node": "CPUSuperClusterMaxFreq",
-      "Duration": 1500,
+      "Duration": 3000,
       "Value": "2200000"
     },
     {
@@ -432,43 +432,43 @@
     {
       "PowerHint": "INTERACTION",
       "Node": "GpuPwrLevel",
-      "Duration": 1500,
+      "Duration": 3000,
       "Value": "0"
     },
     {
       "PowerHint": "INTERACTION",
       "Node": "CciFreq",
-      "Duration": 1500,
+      "Duration": 3000,
       "Value": "1"
     },
     {
       "PowerHint": "LAUNCH",
       "Node": "CPUSuperClusterMaxFreq",
-      "Duration": 1500,
+      "Duration": 2500,
       "Value": "2200000"
     },
     {
       "PowerHint": "LAUNCH",
       "Node": "CPUSuperClusterMinFreq",
-      "Duration": 1500,
+      "Duration": 2500,
       "Value": "2200000"
     },
     {
       "PowerHint": "LAUNCH",
       "Node": "CPUEfficiencyClusterMaxFreq",
-      "Duration": 1500,
+      "Duration": 2500,
       "Value": "2000000"
     },
     {
       "PowerHint": "LAUNCH",
       "Node": "CPUEfficiencyClusterMinFreq",
-      "Duration": 1500,
+      "Duration": 2500,
       "Value": "2000000"
     },
     {
       "PowerHint": "LAUNCH",
       "Node": "MemFreq",
-      "Duration": 1500,
+      "Duration": 2500,
       "Value": "4266000000"
     },
     {
@@ -480,19 +480,19 @@
     {
       "PowerHint": "LAUNCH",
       "Node": "GpuPwrLevel",
-      "Duration": 1500,
+      "Duration": 2500,
       "Value": "0"
     },
     {
       "PowerHint": "LAUNCH",
       "Node": "GpuDvfsTimerMargin",
-      "Duration": 2000,
+      "Duration": 3000,
       "Value": "18743356"
     },
     {
       "PowerHint": "LAUNCH",
       "Node": "GpuDvfsLoadingStep",
-      "Duration": 2000,
+      "Duration": 3000,
       "Value": "771"
     },
     {
diff --git a/configs/properties/vendor.prop b/configs/properties/vendor.prop
index ffce913..37c327f 100644
--- a/configs/properties/vendor.prop
+++ b/configs/properties/vendor.prop
@@ -166,7 +166,7 @@ ro.vendor.mtk_log_hide_gps=1
 # HWUI
 debug.hwui.use_hint_manager=true
 debug.hwui.target_cpu_time_percent=30
-debug.sf.enable_adpf_cpu_hint=true
+debug.sf.enable_adpf_cpu_hint=false
 
 # HWComposer
 ro.vendor.mtk_backlight_hwc_support=0
diff --git a/configs/vintf/manifest.xml b/configs/vintf/manifest.xml
index 35af7d5..a3ebd9b 100644
--- a/configs/vintf/manifest.xml
+++ b/configs/vintf/manifest.xml
@@ -422,6 +422,16 @@
         </interface>
         <fqname>@1.2::INfc/default</fqname>
     </hal>
+    <hal format="hidl">
+        <name>vendor.nxp.nxpnfc</name>
+        <transport>hwbinder</transport>
+        <version>2.0</version>
+        <interface>
+            <name>INxpNfc</name>
+            <instance>default</instance>
+        </interface>
+        <fqname>@2.0::INxpNfc/default</fqname>
+    </hal>
     <hal format="hidl">
         <name>android.hardware.bluetooth</name>
         <transport>hwbinder</transport>
diff --git a/device.mk b/device.mk
index 0ae8733..3969728 100644
--- a/device.mk
+++ b/device.mk
@@ -302,7 +302,6 @@ PRODUCT_COPY_FILES += \
 
 # NFC
 PRODUCT_PACKAGES += \
-    android.hardware.nfc-service.nxp \
     com.android.nfc_extras \
     Tag
 
diff --git a/proprietary-files.txt b/proprietary-files.txt
index 3bf1549..39ed216 100644
--- a/proprietary-files.txt
+++ b/proprietary-files.txt
@@ -1,5 +1,6 @@
 
-# All vendor/lib/ (32-bit) entries removed — 64-bit-only buildvendor/lib64/lib_lvacfs.so
+# All vendor/lib/ (32-bit) entries removed — 64-bit-only build
+vendor/lib64/lib_lvacfs.so
 vendor/lib64/libaedv.so
 vendor/lib64/libaibld.so
 vendor/lib64/libanc_hdr_check.so
@@ -1013,7 +1014,10 @@ vendor/lib64/vendor.mediatek.hardware.neuropilot.agent@1.2.so
 
 # DROPPED (missing): vendor/lib64/libpn557_fw.so
 # NFC
+vendor/bin/hw/android.hardware.nfc_snxxx@1.2-service
+vendor/etc/init/android.hardware.nfc_snxxx@1.2-service.rc
 vendor/etc/init/init.nxp.nfc.rc
+vendor/lib64/nfc_nci_nxp_snxxx.so;DISABLE_CHECKELF
 
 # NVRAM
 vendor/bin/hw/vendor.mediatek.hardware.nvram@1.1-service
diff --git a/rootdir/etc/init/hw/init.mt6789.power.rc b/rootdir/etc/init/hw/init.mt6789.power.rc
index df3494f..d8b47da 100644
--- a/rootdir/etc/init/hw/init.mt6789.power.rc
+++ b/rootdir/etc/init/hw/init.mt6789.power.rc
@@ -21,6 +21,10 @@ on property:vendor.all.modules.ready=1
     write /sys/devices/system/cpu/cpufreq/policy0/scaling_governor "reflex"
     write /sys/devices/system/cpu/cpufreq/policy6/scaling_governor "reflex"
 
+    # Mali GPU DVFS: switch from dummy to simple_ondemand
+    write /sys/devices/platform/soc/13000000.mali/devfreq/13000000.mali/governor "simple_ondemand"
+    write /sys/devices/platform/soc/13000000.mali/devfreq/13000000.mali/polling_interval 100
+
 on init
     write /sys/devices/platform/soc/11270000.ufshci/clkgate_enable 0
     write /proc/sys/kernel/sched_util_clamp_min_rt_default 0
diff --git a/sepolicy/vendor/hal_health_default.te b/sepolicy/vendor/hal_health_default.te
index e46b923..aee506a 100644
--- a/sepolicy/vendor/hal_health_default.te
+++ b/sepolicy/vendor/hal_health_default.te
@@ -2,3 +2,4 @@ allow hal_health_default sysfs_battery:file { getattr open read };
 allow hal_health_default sysfs_battery:dir search;
 allow hal_health_default sysfs_batteryinfo:file { getattr open read };
 allow hal_health_default sysfs_batteryinfo:dir search;
+allow hal_health_default sysfs:file { getattr open read };
