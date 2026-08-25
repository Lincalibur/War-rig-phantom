# CyberDeck — component index

Full plan: `docs/opsec-osint-deck-plan.md`. Parts sourcing: `docs/deck-component-availability.md`.

Each numbered folder is a standalone, individually-testable component. Build and validate each on its own (against serial output / fake data / a real signal source) before touching the integration layer. Only after all four pass their own checklist do you move into `05-integration`.

| # | Component | Device | Depends on | Status |
|---|---|---|---|---|
| 1 | [WiFi Sniffer Node](01-wifi-sniffer/README.md) | ESP32-C3 #1 | — | 🟨 compiles clean, not yet flashed/tested |
| 2 | [BLE Hunter Node](02-ble-hunter/README.md) | ESP32-C3 #2 + small LCD (confirmed: 0.96" I2C SSD1306) | — | 🟨 in progress — 2.1/2.2 confirmed live; 2.3/2.4/2.5 flashed (2 real bugs found+fixed in 2.3), live re-confirm still pending |
| 3 | [Sub-GHz + Control Surface](03-subghz-control/README.md) | Arduino Uno + big LCD | — (LCD part TBD) | 🟨 compiles clean, not yet flashed/tested |
| 4 | [Hub Console](04-hub-console/README.md) | ESP32 + screen | — (screen model TBD) | 🟨 compiles clean, not yet flashed/tested |
| 5 | [Integration Layer](05-integration/README.md) | all of the above | 1, 2, 3, 4 must each be standalone-complete first | ⬜ not started |

## Build order
1. Components 1–4 in parallel or any order — they don't depend on each other.
2. Within 5-integration: bring up ESP-NOW for one C3 node → hub, validate, then the second C3 node.
3. Then UART for the Arduino → hub.
4. Then swap the hub's dashboard from synthetic to live data.
5. Then power + physical enclosure.

## Open decisions blocking full code (not blocking starting the build)
- Exact small-LCD model for the BLE hunter (I2C OLED vs SPI TFT) — see Component 2.
- Exact big-LCD model for the sub-GHz/control node (character LCD vs graphic TFT) — see Component 3.
- Exact hub screen model/pin mapping — see Component 4.

None of these block starting Component 1 (no display) or the core logic of 2/3/4 — just the final display-driver wiring.
