# NFC Configuration Analysis — TECNO CM6 (MT6789)

Device: **TECNO CM6** · Platform: **MediaTek MT6789** · Android **12 (API 31)** · Product: `hal_mgvi_64_nfc_armv82` / `CM6-OP`

---

## 1. NFC Controller

The device uses an **NXP PN553** NFC controller (configured via `/dev/pn553`), driven by the **NXP SNxxx** (SN100/SN110 family) NCI stack.

```
NXP PN553 → /dev/pn553 → nfc_nci_nxp_snxxx.so → android.hardware.nfc@1.2 HIDL HAL
```

## 2. NFC HAL Architecture

Two-layer HIDL HAL:

| Layer | Interface | Instance | Binary/Library |
|-------|-----------|----------|----------------|
| AOSP | `android.hardware.nfc@1.2::INfc` | `default` | `android.hardware.nfc_snxxx@1.2-service` |
| NXP | `vendor.nxp.nxpnfc@2.0::INxpNfc` | `default` | `vendor.nxp.nxpnfc@2.0.so` |

### HAL Service

- **Binary:** `/vendor/bin/hw/android.hardware.nfc_snxxx@1.2-service`
- **Init:** `/vendor/etc/init/android.hardware.nfc_snxxx@1.2-service.rc`
- **Service name:** `vendor.nfc_hal_service`
- **User/Group:** `nfc:nfc`
- **Class:** `hal`

### HAL Shared Libraries

| Library | Description |
|---------|-------------|
| `android.hardware.nfc@1.0.so` | Base NFC HAL interface |
| `android.hardware.nfc@1.1.so` | NFC HAL extension |
| `android.hardware.nfc@1.2.so` | NFC HAL extension (HCE) |
| `vendor.nxp.nxpnfc@2.0.so` | NXP proprietary extensions |
| `nfc_nci_nxp_snxxx.so` | NCI stack implementation for SNxxx |

## 3. Key Configuration Files

### `/vendor/etc/libnfc-nci.conf` (NCI Stack)

| Parameter | Value | Meaning |
|-----------|-------|---------|
| `NFC_DEBUG_ENABLED` | `1` | Debug enabled |
| `NFA_STORAGE` | `/data/nfc` | State data |
| `NFC_POLL_DURATION` | `500` | 500ms polling |
| `POLL_TECH_MASK` | `0x0F` | Poll A+B+F+ISO15693 |
| `UICC_LISTEN_TECH_MASK` | `0x07` | UICC listens on A+B+F |
| `HOST_LISTEN_TECH_MASK` | `0x01` | Host listens on A |
| `SCREEN_OFF_POWER_STATE` | `1` | Full power when screen off |
| `NFA_PREFERRED_EE` | `0x01` | Prefer eSE for tech routing |
| `AID_MATCHING_MODE` | `0x03` | Exact + subset + prefix |
| `NCI_RESET_TYPE` | `0x02` | Keep configs on reset |
| `MAX_EE` | `4` | Max 4 secure elements |
| `NFA_POLL_BAIL_OUT_MODE` | `1` | Recovery polling enabled |

### `/vendor/etc/libnfc-nxp.conf` (NXP HAL)

| Parameter | Value | Meaning |
|-----------|-------|---------|
| `NXP_NFC_DEV_NODE` | `/dev/pn553` | PN553 controller |
| `NXP_NFC_PROFILE_EXTENSION` | `1` | NXP extensions enabled |
| `NXP_MIFARE_READER_ENABLE` | `1` | MIFARE Classic support |
| `DEFAULT_ISO_DEP_ROUTE` | `0x00` | Host |
| `DEFAULT_AID_ROUTE` | `0x00` | Host |
| `DEFAULT_MIFARE_CLT_ROUTE` | `0x01` | eSE |
| `DEFAULT_FELICA_CLT_ROUTE` | `0x01` | eSE |
| `NXP_FW_TYPE` | `0x01` | FW loaded from `.so` library |
| `NXP_FLASH_CONFIG` | `0x02` | Flash on version mismatch |
| `NXP_DUAL_UICC` | `0x01` | Dynamic dual-UICC |
| `NXP_CORE_PROP_EXTN` | `1` | Proprietary extensions |
| `NXP_RDR_DISABLE_ENABLE_LPCD` | `1` | Low-power card detection |
| `EMVCO_CONFIG_FORMAT` | `1` | EMVCo format |
| `HOST_LISTEN_TECH_MASK` | `0x07` | Host listens on A+B+F |
| `OFF_HOST_ESE_PIPE_ID` | `0x16` | eSE pipe |
| `NXP_ISO_DEP_MAX_TRANSCEIVE` | `0xFEFF` | Extended APDU |
| `NXP_GUARD_TIMER` | `15` | 15s guard timer |

### `/vendor/etc/libnfc-nxp_RF.conf` (RF Tuning)

Contains **13 RF configuration blocks** (`NXP_RF_CONF_BLK_1` through `13`) with raw NCI commands for:
- Antenna Load Matching / Phase Load Matching (ALM/PLM) calibration
- Antenna tuning parameters specific to the CM6 antenna design
- Proprietary A0/A1 NCI commands for RF optimization
- System clock: PLL source @ 26 MHz

## 4. Firmware

**No separate firmware binary** (`.bin`) file is present. The PN553 firmware is **embedded inside `nfc_nci_nxp_snxxx.so`**.

Flashing policy: `NXP_FLASH_CONFIG=0x02` — firmware is updated only when the stored version differs from the library version.

## 5. Init Sequence

### Startup Flow

| Order | Script | Action |
|-------|--------|--------|
| 1 | `init.nxp.nfc.rc` (post-fs-data) | Creates `/data/vendor/nfc/` and `/data/vendor/nfc/param/` |
| 2 | `init.nxp.nfc.rc` | Sets `nfc:nfc` ownership on `/dev/pn553` |
| 3 | `android.hardware.nfc_snxxx@1.2-service.rc` | Starts `vendor.nfc_hal_service` (class `hal`) |
| 4 | `factory_init.rc` / `meta_init.rc` / `multi_init.rc` | Import NFC init scripts |
| 5 | `ueventd.rc` | Sets `/dev/st21nfc` → `nfc:radio` (legacy ST reference) |

### Dual NFC Personality

The dump contains **references to both STMicroelectronics and NXP**:

- **STMicro (inactive/legacy):** `ueventd.rc` references `/dev/st21nfc` and `/dev/st54spi`. Some MediaTek init files import `init.stnfc.rc` and `android.hardware.nfc@1.2-service-st.rc`, but these files are **not present** in the dump.
- **NXP (active):** All binaries, libraries, `.conf` files, and the actual HAL service binary are NXP PN553/SNxxx.

The ST references are MediaTek BSP artifacts that were not cleaned up.

## 6. Secure Element Integration

An `android.hardware.secure_element@1.2` HAL is present:

| Binary | Service |
|--------|---------|
| `android.hardware.secure_element@1.2-service-mediatek` | `/vendor/bin/hw/` |
| `android.hardware.secure_element@1.2-service-mediatek.rc` | `/vendor/etc/init/` |

Libraries: `android.hardware.secure_element@1.0/1.1/1.2.so`

Routing configured:
- **ISO-DEP / AID:** Default route → Host (`0x00`)
- **MIFARE CLT:** Route → eSE (`0x01`)
- **Felica CLT:** Route → eSE (`0x01`)
- **eSE pipe:** `0x16`
- **UICC:** Dual UICC with dynamic switching (`NXP_DUAL_UICC=0x01`)

## 7. Permissions & Features

| File | Declares |
|------|----------|
| `android.hardware.nfc.xml` | `android.hardware.nfc`, `android.hardware.nfc.any` |
| `android.hardware.nfc.hce.xml` | `android.hardware.nfc.hce`, `android.hardware.nfc.any` |
| `com.android.nfc_extras.xml` | Library `com.android.nfc_extras.jar` (system/framework) |
| `com.nxp.nfc.xml` | Library `com.nxp.nfc.jar` (system_ext/framework) |
| `com.nxp.mifare.xml` | Feature `com.nxp.mifare` |
| `com.nxp.ls.xml` | Library `com.nxp.ls.jar` (vendor/framework) |

## 8. Build Properties

| Property | Value |
|----------|-------|
| `ro.vendor.tran.midtest.nfc2_support` | `1` (manufacturing mid-test) |

## 9. SELinux

No `.te` policy files are present in the vendor dump (only `file_contexts`).

- NFC HAL runs as `nfc: nfc`
- `/dev/pn553` → `nfc: nfc`
- `/dev/st21nfc` (legacy) → `nfc: radio` (via ueventd.rc)
- `/dev/st54spi` (SE, legacy) → `secure_element` user
- Policy version: 31.0 (Android 11 SELinux)

## 10. Complete File Inventory

```
vendor/
├── bin/hw/
│   ├── android.hardware.nfc_snxxx@1.2-service
│   └── android.hardware.secure_element@1.2-service-mediatek
├── lib64/
│   ├── android.hardware.nfc@1.0.so
│   ├── android.hardware.nfc@1.1.so
│   ├── android.hardware.nfc@1.2.so
│   ├── vendor.nxp.nxpnfc@2.0.so
│   ├── nfc_nci_nxp_snxxx.so
│   ├── android.hardware.secure_element@1.0.so
│   ├── android.hardware.secure_element@1.1.so
│   └── android.hardware.secure_element@1.2.so
├── etc/
│   ├── libnfc-nci.conf
│   ├── libnfc-nxp.conf
│   ├── libnfc-nxp_RF.conf
│   ├── init/
│   │   ├── android.hardware.nfc_snxxx@1.2-service.rc
│   │   ├── init.nxp.nfc.rc
│   │   └── hw/
│   │       ├── init.connectivity.common.rc  (imports init.stnfc.rc)
│   │       ├── factory_init.rc             (imports init.stnfc.rc)
│   │       ├── meta_init.rc                (imports android.hardware.nfc@1.2-service-st.rc)
│   │       └── multi_init.rc               (imports android.hardware.nfc@1.2-service-st.rc)
│   ├── permissions/
│   │   ├── android.hardware.nfc.xml
│   │   ├── android.hardware.nfc.hce.xml
│   │   ├── com.android.nfc_extras.xml
│   │   ├── com.nxp.nfc.xml
│   │   ├── com.nxp.mifare.xml
│   │   └── com.nxp.ls.xml
│   ├── ueventd.rc
│   └── vintf/manifest.xml
└── build.prop
```

## 11. Architecture Diagram

```
┌─────────────────────────────────────────────┐
│  Android NFC App / Settings / NfcService     │
├─────────────────────────────────────────────┤
│  android.hardware.nfc@1.2 HIDL HAL          │
│  (INfc/default)                              │
├─────────────────────────────────────────────┤
│  vendor.nxp.nxpnfc@2.0 HIDL HAL             │
│  (INxpNfc/default — NXP extensions)          │
├─────────────────────────────────────────────┤
│  nfc_nci_nxp_snxxx.so (NCI stack + FW)       │
├─────────────────────────────────────────────┤
│  Kernel Driver → /dev/pn553                  │
├─────────────────────────────────────────────┤
│  NXP PN553 (PN557-clone) NFC Controller      │
├──────────┬──────────┬──────────┬────────────┤
│  Antenna │   eSE    │  UICC 1  │  UICC 2    │
│  (13 RF  │(pipe 0x16)│(SIM1)    │(SIM2)      │
│  blocks) │          │          │            │
└──────────┴──────────┴──────────┴────────────┘
```
