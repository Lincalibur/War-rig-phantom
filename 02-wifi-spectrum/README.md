# Component 2 — WiFi Spectrum Node

**Device:** ESP32-C3 #2 + 0.96" I2C SSD1306 OLED
**Toolchain:** raw `esp_wifi` promiscuous API (no NimBLE)
**Status:** repurposed 2026-09-01 from the original BLE hunter role — see git history for that version.

Sweeps all 13 2.4GHz WiFi channels, measures activity per channel (frame count during a short dwell window), and shows a live bar graph on its own OLED — useful standalone, no hub required. After each full sweep (~2.6s) it also reports the snapshot to the hub over ESP-NOW as `deck_report_t` (node_id=2), same broadcast pattern as the WiFi sniffer node.

## Why the BLE role was dropped

The original BLE hunter (persistence-scoring + AirTag/Find My detection) was flashed and partially confirmed live, but:
- NimBLE + WiFi/ESP-NOW + Adafruit graphics together left only 6% flash headroom (94% used) — effectively no room to extend.
- The feature set had felt lackluster in practice against the project's actual goal (a useful war-driving/recon tool), and WiFi-side options offered more differentiated value for the same board.

Dropping NimBLE also sidesteps a real bug found and fixed the same session it was repurposed: bringing up the BLE controller after pinning the ESP-NOW channel could silently knock the radio off that channel (ESP32-C3 shares one radio between WiFi and BT). The new firmware never touches BLE at all, so that whole class of coexistence problem doesn't apply here anymore.

## Build

- [x] Promiscuous capture across channels 1-13, ~200ms dwell per channel, frame count → normalized 0-255 utilization byte per channel.
- [x] Live OLED bar graph, redrawn once per completed sweep (not per-frame — keeps I2C off the promiscuous RX callback's critical path).
- [x] ESP-NOW report to the hub after every sweep (`chan_util[13]` in `deck_report_t`) + heartbeat on the shared `DECK_HEARTBEAT_INTERVAL_MS` timer.
- [ ] Field test: confirm the bar for a known-busy channel (e.g. temporarily start a phone hotspot on a specific channel) visibly rises relative to its neighbors, both on this board's own OLED and on the hub's Spectrum screen.

## Flash usage

Dropping NimBLE freed most of the headroom the old BLE hunter had eaten — recompile and check `arduino-cli compile` output before assuming a specific number, but expect well under the ~94% the BLE version hit.
