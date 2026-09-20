# CyberDeck v2 firmware

Firmware for the PCB design in `../docs/PCB-DESIGN.md`. Wired-UART architecture: two receive-only field nodes report to an ESP32-S3 hub. The old ESP-NOW build (`../01-*` … `../05-*`) is untouched and superseded.

```
v2/
  shared/deck_link.h        wire protocol: framing, CRC, message structs (nodes + hub)
  shared/deck_node_core.h   node common code: GPS parser, tx queue, UART link, heartbeat
  node_wifi/node_wifi.ino   XIAO ESP32-C5 (or C6): 2.4+5 GHz sniffer
  node_ble/node_ble.ino     XIAO ESP32-C6: BLE scanner
  hub/hub.ino               ESP32-S3: tables, ILI9341 UI, WiGLE CSV logging to SD
```

## Status: written, NOT compiled, NOT flashed

Nothing here has been built yet — no toolchain was reachable when it was written. Expect a round of compile fixes on the first build. In particular check:
- `esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO)` in `node_wifi.ino` exists in your core's IDF version (C5 only).
- The BLE API calls in `node_ble.ino` match your `esp32` core version (core 3.x vs 2.x differ slightly).
- All pin numbers (`DECK_LINK_*`, `DECK_GPS_RX_PIN`, hub `PIN_*`) against the real boards — they come from the PCB draft.

## Build settings

| Sketch | FQBN (arduino-cli) | Notes |
|---|---|---|
| node_wifi | `esp32:esp32:esp32c5:CDCOnBoot=cdc` (or `XIAO_ESP32C6`) | needs an esp32 core with C5 support (3.3+) |
| node_ble | `esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc` | |
| hub | `esp32:esp32:esp32s3:CDCOnBoot=cdc` | use an N8R2/N16R2 devkit, not octal-PSRAM (R8) |

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
- 802.15.4 (Zigbee/Thread) sniffing on the C6.
- Device-side region/country configuration for 5 GHz channels.
