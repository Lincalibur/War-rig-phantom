# Hardware layout

The current build, as of 2026-11-02. The cyberdeck is a kit of separate passive modules. Each module has its own screen and job:

- The `HUB` is an ESP32-DIV v1-style handheld.
- The `WIFI-NODE` and `READOUT` pair forms a big-screen WiFi readout.
- The `STATUS` unit is a removable watch.

One single-cell 18650 pack and a 5 V boost module power them all. Later, the logs go to a laptop for analysis.

The [build page](build-page.html) has the shopping list, wiring and build order. This page records the parts, power and reasons.

![Wiring: ESP32-DIV v1 hub](./wiring.svg)

> The wiring drawing comes from [`cad/wiring/generate_wiring.py`](../cad/wiring/generate_wiring.py). Pins are taken from cifertech's ESP32-DIV firmware, v1 target (`shared.h` and `Libraries/User_Setup v1.h`). Nothing has been wired or tested yet.

**Everything is passive.** The ESP32-DIV firmware also has attack tools (deauth, beacon spam, BLE spoofing, jammers, captive portal). They are not part of this project, and the build has no NRF24 or CC1101 radios to use them with.

---

## Board names

| Name | Board | Job | Firmware |
|---|---|---|---|
| `HUB` | ESP32-WROOM-32U dev board (U.FL antenna) + 2.4" ILI9341 touch/SD TFT + PCF8574 buttons + NEO-7M GPS | Handheld scanner: WiFi and BLE scanning, deauth detection, packet monitor, and GPS wardriving logged to SD as WiGLE CSV | ESP32-DIV v1.7.0, v1 target |
| `WIFI-NODE` | ESP32-C3 (first) | WiFi promiscuous capture: APs, clients, deauths, "following me" persistence. Streams rows to `READOUT` | `v2/proto_readout_c3` (working) |
| `READOUT` | Arduino UNO R3 + 3.5" TFT shield | Big-screen display for `WIFI-NODE` | `v2/proto_readout_uno` (working) |
| `STATUS` | ESP32-C3 (second) + 128×64 OLED | Removable watch. Logs GPS and WiFi scans and sends a CSV to an Android phone. It docks to the kit now and then | Working, on the watch |
| `BLE-SCAN` | ESP32-1732S019 (ESP32 + 1.9" screen) | On hand, **not in this build**. The `HUB` does BLE scanning. Spec: [`BOARD-BLE-SCAN-ESP32-1732S019.md`](BOARD-BLE-SCAN-ESP32-1732S019.md) | — |

Retired: the Raspberry Pi 5 hub, the 7" panel and its RTD2660 driver, the RTL-SDR, and the ESP32-S3 hub. The modules no longer report to a central hub over UART. Each one works on its own.

---

## HUB pins

| Part | Part pin | ESP32 GPIO |
|---|---|---|
| ILI9341 TFT | CS, RESET, DC | 15, 0, 2 |
| | SDI (MOSI), SCK, SDO (MISO) | 13, 14, 12 |
| | LED (backlight, PWM) | 32 |
| XPT2046 touch | T_CS | 33 |
| SD card | SD_CS | 5 |
| Touch + SD, shared bus | T_CLK + SD_SCK, T_DIN + SD_MOSI, T_DO + SD_MISO | 18, 23, 19 |
| PCF8574 | SDA, SCL | 21, 22 |
| NEO-7M GPS | TX, RX | 3 (ESP32 RX), 1 (ESP32 TX) |
| Buttons on PCF8574 | UP, DOWN, LEFT, RIGHT, SELECT | P6, P3, P4, P5, P7, each to GND |

- **GPS on the USB serial pins.** GPIO 1 and 3 are also used by the board's USB serial chip. Unplug the GPS while flashing.
- **PCF8574 on 3V3, not 5 V.** Its address is auto-detected (0x20-0x27).
- **I2C pins.** SDA 21 / SCL 22 are the ESP32 defaults. The firmware calls `Wire.begin()` with no pins, so check this first if the buttons do nothing.
- **No battery gauge.** `BATTERY_ADC_PIN` is -1.
- **Antenna.** A router antenna (RP-SMA) and pigtail go on the board's U.FL socket.

The `WIFI-NODE` → `READOUT` link is a one-way 19200-baud UART. Its wiring is in [`proto-readout-wiring.html`](proto-readout-wiring.html).

---

## Power

```
3x 18650 in parallel (1S3P) -> master switch -> 5V 2A charge/boost module -> 5 V rail
                                                       ^                         |-> HUB: ESP32 VIN, TFT VCC, GPS VCC
                                 USB-C breakout -------+                         |-> READOUT: UNO 5V, WIFI-NODE 5V
                                 (charge input pads)                             +-> STATUS dock (when docked)
```

| Item | Value |
|---|---|
| Cells | 3x 18650 2200 mAh in single holders, wired in parallel |
| Module | 5V 2A charge/boost module for a single Li-ion cell. It has no USB port, so charge it through a USB-C breakout on its input pads |
| Load, estimated | About 600-850 mA at 5 V typical and about 1.4 A peak, for the whole kit with the watch docked |
| Runtime, estimated | About 6 hours. The pack holds about 24 Wh if the cells meet their label |

**Before connecting anything:**
- Charge all three cells to the same voltage before wiring them in parallel. Cells at different voltages dump current into each other when connected.
- Check whether the module cuts out on a low cell. If it doesn't, use protected cells or add a 1S protection board.
- Measure the module's 5 V output with a meter before any board goes on it.
- Wire the battery last. Bring up each part on USB power first.

---

## Case

The 22 × 22 cm clamshell case and its printed filler blocks (`cad/case_filler/`) were designed for the Pi 5 and 7" panel. They don't fit these parts. The layout will be redrawn once the parts are in hand and measured. The Pi 5 filler files stay in the repo for reference, and the measured case numbers still hold:

| Property | Value |
|---|---|
| Outer width | 220 mm |
| Inner width at rim | 205 mm assumed, **not measured** |
| Depth per half | 25 mm, measured |
| Hinge wall to handle cut-out | 165 mm, measured |
| Hinge wall to front wall | 190 mm at the side walls, 210 mm beside the handle |

---

## Open questions

- Is the ESP32 board a full dev board (USB and headers)? This is checked when the parts are collected.
- Does the 5V 2A module cut out on a low cell, and how much current can it really give?
- How should the kit sit in the case? Measure the parts first.
- How do logs get to the laptop? The `HUB` writes WiGLE CSV and PCAP to SD, and `STATUS` sends CSV to the phone. Collecting both on a laptop for analysis is planned but not designed yet.
- NRF24 2.4 GHz spectrum scanning would need an NRF24 module, which nde3d doesn't stock. It's deferred.
