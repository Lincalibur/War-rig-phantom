# Component 2 — BLE Hunter Node

**Device:** ESP32-C3 #2 + small LCD
**Toolchain:** NimBLE-Arduino
**Status:** 🟨 in progress — 2.1/2.2 confirmed working live; 2.3-2.5 code written and flashed (incl. two real bugs found+fixed, see note below), live flag-trigger still not yet confirmed

Continuously scans BLE advertisements and flags devices that keep reappearing across time/location — the signature of an unwanted tracker (AirTag/Tile/SmartTag) rather than a stationary beacon.

## Build in this order

- [x] **2.1 Active BLE scan loop** — short scan windows (scan 3s every 5s) to balance coverage vs power.
  - Test: confirm raw scan lists nearby devices with sane RSSI. ✅ confirmed live (2026-08-13) — caught a phone at rssi=-88.
- [x] **2.2 MAC/RSSI log with time buckets** — record `(mac, rssi, timestamp)`, group into ~10-min sighting windows.
  - Test: log shows sightings grouped correctly across windows. ✅ confirmed live (2026-08-13) — tracked phone MAC `68:cc:12:49:35:0c`, table format correct.
- [ ] **2.3 Persistence scoring** — flag a MAC as suspicious if seen in N+ distinct windows over a span (e.g. 3+ windows/hour) while moving.
  - Implemented: rolling 8-window history per MAC, flags at 3+ distinct windows within trailing 6-window (~1hr) span. Code reviewed and flashed to the board (production 10-min WINDOW_MS).
  - **KNOWN BUG, found and fixed 2026-08-13, still needs live re-confirmation:** the real reason 2.3 could never fire wasn't an RF/environment issue as first suspected — it was two separate NimBLE-Arduino duplicate-advertisement filters, both silently on by default, that meant any given MAC could only ever be reported to `onResult()` ONCE for the life of the scan object, so `sightings` got stuck at 1 forever and persistence scoring could never accumulate. Fixed in `firmware/ble_scan/ble_scan.ino` setup():
    1. `pScan->setScanCallbacks(new ScanCallbacks(), true)` — the `wantDuplicates` arg defaults to `false`.
    2. `pScan->setDuplicateFilter(0)` — a **separate**, controller-level filter (`filter_duplicates` in NimBLE's internal scan params) that defaults to `1` and drops repeat advertisements before they even reach the software callback layer; disabling (1) alone is not enough without also disabling (2).
  - Both fixes are flashed to the board. Live re-test attempted 2026-08-13 with the field's actual BLE traffic and a couple of manually-triggered nearby devices, but no test source advertised reliably/continuously enough during the session to walk a MAC through 3+ distinct windows — genuinely unconfirmed, not contradicted. Real test still needed: put a Bluetooth accessory in **pairing mode** (blinking indicator — pairing mode advertises far more aggressively/reliably than an idle paired device or an app left merely "open") near the board for ~20-30 min (3x real 10-min windows) and confirm the `*** SUSPICIOUS ***` alert fires, and that a device seen only once or twice does not.
- [ ] **2.4 Rotating-MAC awareness** — secondary heuristic matching Apple Find My manufacturer-data payload pattern, since AirTags rotate BLE address.
  - Implemented: matches company ID 0x004C + type byte 0x12 in manufacturer data, flags on sight (doesn't wait for MAC persistence). Flashed, compiles clean. Heuristic payload shape sourced from open-source AirTag-detection projects, not an Apple spec doc.
  - **Not yet confirmed live** — needs an actual AirTag/Find My accessory to test against; none available during today's session.
- [ ] **2.5 RSSI "getting warmer" mode** — once flagged, switch to fast-poll direction-finding, live bar/needle on small LCD.
  - Implemented for the confirmed part: 0.96" I2C SSD1306 OLED (SDA=GPIO8, SCL=GPIO9, addr 0x3C — confirm against board silkscreen). Flashed; confirmed the board boots and runs fine with the OLED still unwired (`display.begin()` fails gracefully, doesn't hang the scan loop).
  - Test still needed: wire the OLED, then physically walk toward/away from a flagged device, confirm the readout tracks direction sensibly.

## Small LCD — decide wiring before 2.5
- [ ] Confirm exact part on hand (I2C OLED SSD1306 vs SPI TFT ST7735/ST7789) — **flag model to get exact pin mapping/driver.**
- [ ] Non-blocking display update (no `delay()` in the render path) so it doesn't stall the scan loop.
  - Test: LCD updates live without flicker/blocking, scan loop keeps running.

## Standalone-complete checklist
- [ ] All sub-tasks above pass their individual test
- [ ] LCD renders persistence/RSSI view without stalling scans
- No hub link yet — see `../05-integration` for ESP-NOW wiring once this is solid standalone.
