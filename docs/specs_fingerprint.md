# Tab5 — measured hardware fingerprint

Everything here was **read off this specific unit**, not copied from a datasheet:
from the full flash backup (`original_flash/tab5-stock-backup.tar.gz`) and from a
read-only capture of the stock firmware's boot console. Nothing was written to
either chip.

Captured 2026-09-02. Unit MAC `80:f1:b2:d1:41:b2`.

Where this disagrees with the published Tab5 specs, **this file is right** — see
[Discrepancies](#discrepancies).

## Silicon

| Property | Value | Source |
|---|---|---|
| SoC | ESP32-P4, revision v1.3 | esptool |
| Cores | 2 x 400 MHz RISC-V + LP core | datasheet |
| Crystal | 40 MHz | esptool |
| Min / max chip rev supported | v0.1 / v1.99 | boot log |
| Flash | 16 MB, mfr `0x46`, dev `0x4018` | esptool |
| PSRAM | 256 Mbit (32 MB), x16, gen 4, 2048-byte burst, 14-cycle read latency | boot log |
| RTC RAM | 31 KiB free at `0x5010809C` | boot log |

## Stock firmware identity

| Property | Value |
|---|---|
| Project | `m5stack_tab5` |
| Version | `639d9cc` (git short hash) |
| Built | Mar 25 2026, 11:51:30 |
| ESP-IDF | v5.4.2 |
| Source | <https://github.com/m5stack/M5Tab5-UserDemo> @ `639d9ccf3b87ed3962d3475ce8584e0d3096cf85` |
| App image size | 5,751,760 bytes (5.49 MiB) — in a 10 MB partition |

Bootloader sits at **`0x2000`**, correct for ESP32-P4. Offset `0x0` is erased
(`0xFF`) by design — not a truncated dump.

## Stock partition table

| Name | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | `0x9000` | 24K |
| phy_init | data/phy | `0xF000` | 4K |
| factory | app | `0x10000` | 10M |
| human_face_det | data/spiffs | `0xA10000` | 400K |
| storage | data/spiffs | `0xA74000` | 2M |

## Peripherals

All confirmed by driver init messages in the stock boot log.

| Function | Part | Detail |
|---|---|---|
| Touch | **ST7123** | FW `1.80.1.16`, 720x1280, 10 points |
| Display | 5" MIPI-DSI IPS | 1280x720 |
| IMU | BMI270 | ODR/range set at init |
| Power monitor | INA226 | |
| Audio in | ES7210 | 4 mics, TDM, 16-bit, slave mode |
| Audio out | ES8388 | mode 2 |
| Camera | SC2356 (MIPI-CSI) | `hal: camera init` |
| RTC | on-board, battery-backed | `rtc bat init: 0x1F` |

## ESP32-C6 Wi-Fi co-processor

The P4 has no radio. Networking runs over SDIO to an ESP32-C6-MINI-1U through
ESP-Hosted.

| Property | Value |
|---|---|
| Slave chip ID | 12 (ESP32-C6) |
| Host chip ID | 18 (ESP32-P4) |
| ESP board type | 13 |
| Capabilities | `0x0D` — WLAN, HCI over SDIO, BLE only |
| Transport | SDIO, 4-bit, 40 MHz, streaming mode |
| Function 0/1 blocksize | 512 / 512 |
| Host BT support | disabled in this build |
| **Slave FW version** | **not reported by this ESP-Hosted build** |

### SDIO pinout (P4 side)

| Signal | CLK | CMD | D0 | D1 | D2 | D3 | Slave reset |
|---|---|---|---|---|---|---|---|
| GPIO | 12 | 13 | 11 | 10 | 9 | 8 | 15 |

### The C6 firmware cannot be backed up from here

Its flash is physically separate and reachable only over SDIO, which has no
read-back path. ESP-Hosted can *push* a slave update (write-only OTA); it cannot
dump one. Reading or writing that chip needs a **USB-TTL adapter on the reserved
download pads** on the PCB — not the P4's USB-C port.

Recovery, if it is ever wiped, is M5Burner's published factory Wi-Fi firmware:
<https://docs.m5stack.com/en/guide/restore_factory/m5tab5_c6_wifi>

Since no version string is emitted, the capability word, board type and chip ID
above are this C6's only identity. If a future host build refuses to talk to it,
suspect an ESP-Hosted version mismatch first and compare against these values.

## Discrepancies

| Item | Published spec | This unit |
|---|---|---|
| Touch controller | GT911 | **ST7123**, FW `1.80.1.16` |

Write the touch driver against ST7123.

## Regenerating this

```
# flash + partition facts
esptool flash_id            # -p COM16 if more than one board is attached
tar -xzOf original_flash/tab5-stock-backup.tar.gz tab5-backup-full.bin > flash.bin
gen_esp32part.py <(dd if=flash.bin bs=1 skip=32768 count=3072)
esptool --chip esp32p4 image_info <(dd if=flash.bin bs=4096 skip=16 count=2560)

# peripheral + C6 facts: reset the P4 and read its console at 115200
# full capture is archived as stock-boot.log inside the backup tarball
```
