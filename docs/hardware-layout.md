# Hardware layout

The current build, as of 2026-11-13. The cyberdeck is a kit of separate modules. Each module has a
**fixed proper name** (so references don't drift when a module's role changes) and its own screen and job:

- **SPECTRE** is the active, phone-driven ESP32-DIV handheld.
- **SENTINEL** is the passive, always-on watcher (an ESP32-C3 + UNO pair). See [`watcher-plan.md`](watcher-plan.md).
- **BEACON** is the removable logging watch.

One single-cell 18650 pack and a 5 V boost module power them all. Later, the logs go to a laptop for analysis.

The [build page](build-page.html) has the shopping list, wiring and build order. This page records the parts, power and reasons.

![Wiring: ESP32 + touchscreen + GPS](./wiring.svg)

> The wiring drawing comes from [`cad/wiring/generate_wiring.py`](../cad/wiring/generate_wiring.py). Pins are taken from cifertech's ESP32-DIV firmware, v1 target (`shared.h` and `Libraries/User_Setup v1.h`). The GPS wires straight to the ESP32 — there is no protoboard and no button matrix; the handheld is driven from the CONSOLE phone app over BLE (and the touchscreen). Nothing has been wired or tested yet.

**Full ESP32-DIV capability (2026-11-10).** The HUB now targets the complete ESP32-DIV V1 feature set, including the active tools (WiFi deauth, beacon/probe flood, captive/evil portal, BLE spoof/spam and jammer), plus the NRF24 (2.4 GHz jam/spoof/ESB) and CC1101 (sub-GHz capture/replay/jam) radios.

> **Authorized testing only.** The active tools transmit. Use them **only** against hardware you own and control — the project's own test laptop and spare router on an isolated bench. Do not transmit against networks, devices or people you don't own. Deauth/jamming is illegal in most jurisdictions when aimed at others.

---

## Module names

Each **name** is tied to the physical hardware and stays fixed; the **role** is what it currently does
and may change. (The old role-based labels `HUB`/`WIFI-NODE`/`READOUT`/`STATUS`/`BLE-SCAN` are kept as
aliases so older docs still make sense.)

| Name | Was | Board | Role now | Firmware |
|---|---|---|---|---|
| **SPECTRE** | `HUB` | ESP32-WROOM-32U dev board (U.FL antenna) + 2.4" ILI9341 touch/SD TFT + NEO-7M GPS (phone-app control, no button matrix) + ESP32-DIV radio shield (3× NRF24L01 + CC1101) | **Active, phone-driven power tool.** Full ESP32-DIV multi-tool: WiFi scan/deauth/evil-portal/packet-monitor, BLE scan/spoof/jam, 2.4 GHz (NRF24) jam/spoof/ESB, sub-GHz (CC1101) capture/replay/jam, GPS wardriving to SD. The orchestrator is being ported here ([`hub-migration-plan.md`](hub-migration-plan.md)) | stock ESP32-DIV v1.7.2 today → ported orchestrator |
| **SENTINEL** | `WIFI-NODE` + `READOUT` | ESP32-C3 + Arduino UNO R3 / 3.5" TFT (soldered pair, one-way 19200 UART) | **Passive, always-on watcher** (never transmits): TRACKERS (following BLE tags), TAILS (persistent WiFi clients), ATTACKS (deauth floods, evil-twin APs). Alerts on screen + red LED. SENTINEL-C3 = brain, SENTINEL-TFT = screen | `sentinel/v1/{c3,uno}` (compile-verified 2026-11-13) |
| **BEACON** | `STATUS` | ESP32-C3 + 128×64 OLED | Removable logging watch. Logs GPS + WiFi scans and sends CSV to an Android phone; docks to the kit now and then | Working, on the watch |
| **CONSOLE** | — | Android phone app (Flutter) | Controls SPECTRE over BLE. `app/` | Built: `CyberDeck.apk` |
| **RELAY** | `BLE-SCAN` | ESP32-1732S019 (ESP32 + 1.9" screen) | Spare / utility, **not in this build**. Spec: [`BOARD-BLE-SCAN-ESP32-1732S019.md`](BOARD-BLE-SCAN-ESP32-1732S019.md) | — |

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
| NEO-7M GPS | TX, RX | 3 (ESP32 RX), 1 (ESP32 TX) |
| NRF24 #1 (primary) | CE, CSN | 4, 5 |
| NRF24 #2 | CE, CSN | 26, 27 |
| NRF24 #3 (scanner/ESB/MouseJack) | CE, CSN | 16, 17 |
| CC1101 (sub-GHz) | CS, GDO0, GDO2 | 27, 26, 16 |
| Radios shared SPI | SCK, MOSI, MISO | 18, 23, 19 (shared with SD + touch) |

- **GPS wired straight to the ESP32.** VCC to 3V3, GND to GND, TXD to GPIO 3, RXD (optional) to GPIO 1, PPS unconnected. No protoboard. Feed VCC from 5 V only if it won't get a fix.
- **GPS on the USB serial pins.** GPIO 1 and 3 are also used by the board's USB serial chip. Unplug the GPS (or pull its TXD lead off GPIO 3) while flashing.
- **No button matrix.** The PCF8574 and the five buttons were dropped; the handheld is driven from the CONSOLE phone app over BLE and the touchscreen. GPIO 21/22 are free.
- **No battery gauge.** `BATTERY_ADC_PIN` is -1.
- **Antenna.** A router antenna (RP-SMA) and pigtail go on the board's U.FL socket.

The `WIFI-NODE` → `READOUT` link is a one-way 19200-baud UART. Its wiring is in [`proto-readout-wiring.html`](proto-readout-wiring.html).

---

## Radio shield (NRF24 + CC1101)

The radios come from cifertech's ESP32-DIV V1 shield: **3× NRF24L01+** (2.4 GHz jam/spoof/ESB/MouseJack) and **1× CC1101** (sub-GHz). Pins are fixed by the stock firmware's `shared.h`, so wire to the GPIOs above and flash the unmodified V1 binary — no source changes.

- **One shared SPI bus, time-shared.** The SD card, touch, all three NRF24 and the CC1101 sit on SCK 18 / MOSI 23 / MISO 19. They are not all active at once: the firmware reclaims the bus and re-mounts the SD card after a radio tool runs. By design, **GPIO 5 is both `SD_CS` and NRF24 #1 `CSN`** (SD or NRF24 #1, not both at the same instant), and **GPIO 26/27/16 are reused** between NRF24 #2/#3 and the CC1101, so 2.4 GHz-NRF tools and sub-GHz tools run one mode at a time. This is cifertech's own multiplexing, not a wiring error.
- **NRF24 is 3V3 only.** Power each module from 3V3, not 5 V. Put a **10 µF** (plus a 100 nF) cap right across each NRF24's VCC/GND — they brown out and misbehave without it, and three radios on one rail is demanding. NRF24L01+PA+LNA modules give more range but draw more; budget the power accordingly (see the power table — radios were not in the original load estimate).
- **CC1101 band.** Pick the module for the band you'll test: **433 MHz** is the common SA ISM band for remotes/sensors; 868/915 MHz is the other. Decoupling as above.
- **Antennas.** Each NRF24 and the CC1101 wants its own antenna. Keep them physically separated — the earlier build hit near-field interference between close 2.4 GHz radios.
- **Not yet verified against the schematic.** The pin table is taken from the firmware source (`shared.h`, v1 branch). Cifertech's `Schematic/v1/ESP32DIV-SHIELD.jpg` + `ESP32DIV-SHIELD-BOM.pdf` are the physical reference; cross-check them before soldering the shield.

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

The 22 × 22 cm clamshell case and its printed filler blocks (`cad/case_filler/`) were designed for the Pi 5 and 7" panel. They don't fit these parts. The module layout below is the current plan — schematic, not to scale; the foam compartments get cut once the parts are measured. The Pi 5 filler files stay in the repo for reference, and the measured case numbers still hold.

The whole **bottom section** holds power: the powerbank, the three 18650 cells and the charging station. **Two power lines** run up the right-hand side into the **top section**, where a **USB hub** sits across the middle with its ports facing up. Each module above the hub (SPECTRE, SENTINEL, BEACON) plugs in with its own power cable — SENTINEL uses two (C3 + UNO). The phone (CONSOLE) is carried by the operator, not stored in the case.

![Module layout in the storage case](./case-layout.svg)

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
- Source the radio shield: 3× NRF24L01 (nde3d doesn't stock them — check another supplier), 1× CC1101 at 433 MHz, antennas, and the decoupling caps. Or buy cifertech's assembled V1 shield.
- Cross-check the radio pin table against cifertech's `Schematic/v1` JPGs before soldering.
- Power: fold the three NRF24 + CC1101 into the 5 V/3V3 load estimate and re-check runtime.
