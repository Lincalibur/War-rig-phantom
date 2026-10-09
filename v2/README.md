# CyberDeck v2 firmware

Firmware for the v2 PCB (`../docs/OVERVIEW.md`, pins in `../docs/PCB-BOM-AND-NETLIST.md`). Wired-UART architecture: two receive-only field nodes report to an ESP32-S3 hub.

> **Board change pending:** this code was written for XIAO ESP32-C5/C6 nodes. The 2026-09-25 BOM switched to an **ESP32-C3 PRO Mini** (WiFi, 2.4 GHz only) and an **ESP32-H2 SuperMini** (BLE). Node pins, board targets and the C5-only 5 GHz call must be updated before the first build.

```
v2/
  shared/deck_link.h        wire protocol: framing, CRC, message structs (nodes + hub)
  shared/deck_node_core.h   node common code: GPS parser, tx queue, UART link, heartbeat
  node_wifi/node_wifi.ino   WiFi sniffer node (target: ESP32-C3 PRO Mini; code still XIAO C5)
  node_ble/node_ble.ino     BLE scanner node (target: ESP32-H2 SuperMini; code still XIAO C6)
  hub/hub.ino               ESP32-S3: tables, ILI9341 UI, WiGLE CSV logging to SD

  # Bench prototypes (spare parts, no custom PCB) — see "Bench prototypes" below
  proto_readout_c3/         ESP32-C3: WiFi-promiscuous scanner -> streams rows over UART
  proto_readout_uno/        Arduino UNO + 3.5" TFT: renders the readout (pairs with proto_readout_c3)
  proto_link_analyzer/      Arduino UNO: link diagnostic (byte/line/valid-frame stats) — bring-up tool
  proto_tracker_tripwire/   ESP32-S3 hub board alone: standalone BLE tracker detector (unflashed)
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

## Status: written, NOT compiled, NOT flashed

Nothing here has been built yet — no toolchain was reachable when it was written. Expect a round of compile fixes on the first build. In particular check:
- `esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO)` in `node_wifi.ino` is C5-only; remove it for the C3.
- The BLE API calls in `node_ble.ino` match your `esp32` core version (core 3.x vs 2.x differ slightly).
- All pin numbers (`DECK_LINK_*`, `DECK_GPS_RX_PIN`, hub `PIN_*`) against `../docs/PCB-BOM-AND-NETLIST.md` and the real boards. Proposed node pins: C3 link TX=GPIO7, RX=GPIO6, GPS RX=GPIO20; H2 pins still to be chosen from its silkscreen.

## Build settings

| Sketch | FQBN (arduino-cli) | Notes |
|---|---|---|
| node_wifi | `esp32:esp32:esp32c3:CDCOnBoot=cdc` | ESP32-C3 PRO Mini. Remove the C5-only `esp_wifi_set_band_mode` call |
| node_ble | `esp32:esp32:esp32h2:CDCOnBoot=cdc` | ESP32-H2 SuperMini. Confirm BLE scanning works on H2 in core 3.x; fallback is a C3 SuperMini |
| hub | `esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi` | ESP32-S3-DevKitC-1 N16R8 (octal PSRAM; GPIO33-37 unused) |

`CDCOnBoot=cdc` is required on the nodes: hardware UART0 is used for the GPS, so `Serial` must be USB.

Hub libraries: `Adafruit GFX Library`, `Adafruit ILI9341`.

## Bring-up order
1. **Hub alone**: flash, confirm screen + SD, all pages render with zero data.
2. **One node at a time** on the bench: the node prints a status line over USB every 2 s; connect its UART to the hub and confirm the hub's `STATUS` page shows LIVE and a non-zero fps.
3. **GPS**: connect the module, confirm `gps=fix` on a node's serial output outdoors, then `GPS FIX` on the hub header.
4. **Log check**: drive/walk a short loop, pull the SD, open `/wardrive/wigle_NNNN.csv`, and try a WiGLE upload.

## Wire protocol
See `shared/deck_link.h`. Frame `A5 5A | type | len | payload | crc16`. Nodes report each AP/client/BLE device when it is new, when its info changes, when it is heard ≥6–8 dB stronger, and every 30–45 s while still visible.

## Not implemented yet
- CC1101 sub-GHz (pins reserved on the hub; CS held high).
- Battery voltage on GPIO6 (VBAT divider). The IP5310 board has no low-battery pin, so this is the only warning.
- 802.15.4 (Zigbee/Thread) sniffing on the C6.
