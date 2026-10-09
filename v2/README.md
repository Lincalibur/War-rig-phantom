# CyberDeck v2 firmware

Firmware for the clamshell build (plan and wiring in [`../docs/hardware-layout.md`](../docs/hardware-layout.md)). Wired-UART architecture: two receive-only nodes report to an ESP32-S3 hub, and the hub draws on a UNO + 3.5" TFT.

> **Retarget pending.** `hub`, `node_wifi` and `node_ble` were written for the shelved single-PCB design (XIAO C5/C6 nodes, 2.8" ILI9341 on the hub) and have never been compiled. The table below says what each one has to become for budget v1.

## Boards and sketches

Board names are defined in [`../docs/hardware-layout.md`](../docs/hardware-layout.md#board-names).

| Board | Hardware | Sketch today | Work needed for budget v1 |
|---|---|---|---|
| `HUB` | ESP32-S3-DevKitC-1 N16R8 | `hub/` | Replace the ILI9341 UI with a row-stream sender for `READOUT`; add LED + piezo alerts; add CC1101 receive |
| `READOUT` | Arduino UNO R3 + 3.5" TFT shield | `proto_readout_uno/` (working) | None for display. Logging belongs on the `HUB` |
| `WIFI-NODE` | ESP32-C3 | `node_wifi/`; capture code proven in `proto_readout_c3/` | Retarget `node_wifi` to the C3: remove the C5-only band call, set the pins |
| `BLE-SCAN` | ESP32-1732S019 (built-in 1.9" ST7789) | `node_ble/`; detection logic in `proto_tracker_tripwire/` | Port to classic ESP32, add the ST7789 screen (LovyanGFX), prefer NimBLE. Spec: [`../docs/BOARD-BLE-SCAN-ESP32-1732S019.md`](../docs/BOARD-BLE-SCAN-ESP32-1732S019.md) |
| `STATUS` | ESP32-C3 + 128×64 OLED | none yet | New standalone sketch: counters on the OLED |

```
v2/
  shared/deck_link.h        wire protocol: framing, CRC, message structs (nodes + hub)
  shared/deck_node_core.h   node common code: GPS parser, tx queue, UART link, heartbeat
  node_wifi/node_wifi.ino   WIFI-NODE (target: ESP32-C3; code still XIAO C5)
  node_ble/node_ble.ino     BLE-SCAN (target: ESP32-1732S019; code still XIAO C6, no screen)
  hub/hub.ino               HUB, ESP32-S3: tables, WiGLE CSV logging to SD (UI still ILI9341)

  # Bench prototypes (spare parts, no custom PCB) — see "Bench prototypes" below
  proto_readout_c3/         ESP32-C3: WiFi-promiscuous scanner -> streams rows over UART
  proto_readout_uno/        Arduino UNO + 3.5" TFT: renders the readout (pairs with proto_readout_c3)
  proto_link_analyzer/      Arduino UNO: link diagnostic (byte/line/valid-frame stats) — bring-up tool
  proto_tracker_tripwire/   ESP32-S3 + ILI9341: standalone BLE tracker detector (unflashed; source for BLE-SCAN)
```

## Bench prototypes (2026-10-05)

Working counter-surveillance prototypes built from spare parts, independent of the custom PCB.
Wiring reference: [`../docs/proto-readout-wiring.html`](../docs/proto-readout-wiring.html).

**`proto_readout_c3` + `proto_readout_uno` — big-screen WiFi recon readout (WORKING).**
An ESP32-C3 (SuperMini) runs 802.11 promiscuous: APs (beacons/probe-resp), clients (probe-req +
data frames), deauth counting, channel hop 1-13. It keeps the tables and diffs the screen, then
streams `R<rr><c><text>` row-draw commands over a one-way 19200-baud UART to an Arduino UNO +
3.5" TFT, which is a dumb terminal (no tables -> the UNO's 2 KB RAM is never the limit). Clients
are sorted by **persistence** = the longest-shadowing unknown device is the "following me"
candidate. Flash the C3 first (`esp32:esp32:esp32c3:CDCOnBoot=cdc`), then the UNO
(`arduino:avr:uno`); unplug the C3->D0 wire while flashing the UNO.

**`proto_link_analyzer` — UNO link diagnostic.** Flash instead of the readout to measure the link:
bytes/s, lines, valid frames, control-byte (garbage) count, raw stream. Keep for future bring-up.

**`proto_tracker_tripwire` — standalone BLE tracker detector (UNFLASHED).** Runs on the S3 hub
board alone (2.8" ILI9341 + buzzer/LED) and flags AirTag/Find My/Tile/SmartTag by persistence.
BLE here uses Bluedroid; if it's flaky, switch to NimBLE (see learnings).

### Hard-won learnings from bring-up
- **AVR printf has no `*` (dynamic width) support.** `snprintf(..., "%-*.*s", COLS, COLS, s)` renders
  *nothing* on the UNO with no error — it silently broke `drawRow` across many rebuilds and looked
  like a link/power/C3 fault. Use literal widths on AVR: `"%-40.40s"`. (ESP32 printf is fine with `*`.)
- **Pace the UNO link.** The UNO's 64-byte hardware RX buffer overflows if the C3 bursts rows faster
  than the slow parallel TFT can paint them -> corrupted lines. The C3 waits ~150 ms between rows and
  does full refreshes rarely (15 s); normal updates send only the one changed status row.
- **BLE on the ESP32-C3 is unreliable** (Bluedroid + WiFi coexistence on the single radio crashes the
  chip). The C3's prototype role is WiFi-only. For BLE, prefer NimBLE or a separate chip (S3/H2).
- **C3 USB-CDC serial can't be captured** via `arduino-cli monitor` on the debian server — use the
  UNO link as the monitor instead.

## Status: hub and nodes written, NOT compiled, NOT flashed

The bench prototypes above run; `hub`, `node_wifi` and `node_ble` have never been built. Expect a round of compile fixes on the first build. In particular check:
- `esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO)` in `node_wifi.ino` is C5-only; remove it for the C3.
- The BLE API calls in `node_ble.ino` match your `esp32` core version (core 3.x vs 2.x differ slightly).
- All pin numbers (`DECK_LINK_*`, `DECK_GPS_RX_PIN`, hub `PIN_*`) against the real boards. Hub SPI, CC1101 and node-link pins are in section 3.1 of `../docs/PCB-BOM-AND-NETLIST.md`. Proposed, not yet in code: `WIFI-NODE` link TX=GPIO7, RX=GPIO6, GPS RX=GPIO20; `BLE-SCAN` link TX=GPIO17, RX=GPIO16, GPS RX=GPIO22; `HUB` readout TX=GPIO9, buzzer=GPIO18, alert LED=GPIO21.
- The hub's display pins (GPIO7-10), nav buttons (GPIO38-42) and VBAT sense (GPIO6) belong to the shelved PCB and are unused in this build.

## Build settings

| Sketch | FQBN (arduino-cli) | Notes |
|---|---|---|
| node_wifi | `esp32:esp32:esp32c3:CDCOnBoot=cdc` | `WIFI-NODE`, ESP32-C3. Remove the C5-only `esp_wifi_set_band_mode` call |
| node_ble | `esp32:esp32:esp32` | `BLE-SCAN`, ESP32-1732S019 ("ESP32 Dev Module", 4 MB flash, CH340). UART0 is the USB serial port on this board, so the link and GPS need UART1 and UART2 |
| proto_readout_uno | `arduino:avr:uno` | `READOUT`. Libraries: MCUFRIEND_kbv, Adafruit GFX |
| hub | `esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi` | ESP32-S3-DevKitC-1 N16R8 (octal PSRAM; GPIO33-37 unused) |

`CDCOnBoot=cdc` is required on the C3 node: hardware UART0 is used for the GPS, so `Serial` must be USB.

Hub libraries today: `Adafruit GFX Library`, `Adafruit ILI9341`. Both go away once the hub draws on `READOUT`.

## Bring-up order
0. **Power first**: bench-test the 2S pack, charger (8.4 V) and buck (5.1 V) before any board is connected. See `../docs/hardware-layout.md` section 5.
1. **Hub alone**: flash, confirm SD and the `READOUT` link, all pages render with zero data.
2. **One node at a time** on the bench: the node prints a status line over USB every 2 s; connect its UART to the hub and confirm the hub's status page shows LIVE and a non-zero fps.
3. **GPS**: connect the module's TX to both nodes, confirm `gps=fix` on a node's serial output outdoors, then `GPS FIX` on the hub header.
4. **Log check**: drive/walk a short loop, pull the SD, open `/wardrive/wigle_NNNN.csv`, and try a WiGLE upload.

## Wire protocol
See `shared/deck_link.h`. Frame `A5 5A | type | len | payload | crc16`. Nodes report each AP/client/BLE device when it is new, when its info changes, when it is heard ≥6–8 dB stronger, and every 30–45 s while still visible.

## Not implemented yet
- CC1101 sub-GHz (pins reserved on the hub; CS held high).
- LED + piezo alerts and the persistence ("following me") scoring on the hub.
- Pack voltage sensing. The 2S capacity indicator board is the only battery readout for now.
- 802.15.4 (Zigbee/Thread): no board in this build has that radio.
