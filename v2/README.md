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
```

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
