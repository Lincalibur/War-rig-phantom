# CyberDeck — component index

Full plan: `docs/opsec-osint-deck-plan.md`. Parts sourcing: `docs/deck-component-availability.md`.

Each numbered folder is a standalone, individually-testable component. Build and validate each on its own (against serial output / fake data / a real signal source) before touching the integration layer. Only after all four pass their own checklist do you move into `05-integration`.

| # | Component | Device | Depends on | Status |
|---|---|---|---|---|
| 1 | [WiFi Sniffer Node](01-wifi-sniffer/README.md) | ESP32-C3 #1 | — | 🟨 compiles clean, not yet flashed/tested |
| 2 | [WiFi Spectrum Node](02-wifi-spectrum/README.md) | ESP32-C3 #2 + 0.96" I2C SSD1306 | — | 🟨 repurposed from the original BLE hunter role 2026-09-01 (see component README for why), flashed, field-confirmation pending |
| 3 | [Sub-GHz Node](03-subghz-control/README.md) | ESP32-C3 #3, no display | — | 🟨 compiles clean, not yet flashed/tested |
| 4 | [Hub Console](04-hub-console/README.md) | ESP32 + screen | — (screen model TBD) | 🟨 compiles clean, not yet flashed/tested |
| 5 | [Integration Layer](05-integration/README.md) | all of the above | 1, 2, 3, 4 must each be standalone-complete first | ⬜ not started |

**ESP-NOW-consolidated redesign** (`esp-now-consolidation` branch): the Arduino Uno + big LCD control surface has been dropped. Sub-GHz sensing moves to a third ESP32-C3 node reporting over ESP-NOW like the WiFi/BLE nodes; all display/menu/control responsibilities live only on the hub. The old Uno sketch remains in git history / on `main` if needed. Antennas (U.FL external antenna boards for the 2.4GHz nodes) are deferred to a later pass — this redesign targets the standard trace-antenna boards already on hand.

## Build order
1. Components 1–4 in parallel or any order — they don't depend on each other.
2. Within 5-integration: bring up ESP-NOW for each of the three C3 nodes → hub, one at a time.
3. Then swap the hub's dashboard from synthetic to live data.
4. Then power + physical enclosure (portable, per the wardriving use case — LiPo+boost prioritized over the bench power-bank plan).

## Open decisions blocking full code (not blocking starting the build)
- Exact hub screen model/pin mapping — see Component 4 (resolved: ESP32-1732S019 integrated ST7789).

Component 2's display is resolved (0.96" I2C SSD1306).
