# Component 4 — Hub Console

**Device:** ESP32 + attached screen
**Status:** 🟨 `firmware/hub_console/hub_console.ino` (steps 4.1-4.4, synthetic data + ESP-NOW/UART receivers wired) compiles clean against `esp32:esp32:esp32`, not yet flashed/tested on hardware

Central aggregator: receives data from both C3 nodes and the Arduino, renders a unified dashboard, optionally logs to SD.

## Build in this order — build the UI against FAKE data first, real links come later

- [ ] **4.1 Dashboard UI skeleton, synthetic data** — overview screen (counts per node), WiFi detail list, BLE detail list + tracker alerts, sub-GHz detail list, node health (last-seen timestamp per node).
  - Test: menu system, screens, and navigation are solid using hardcoded/fake sample data — no radio link needed yet.
  - Confirm exact screen model/pin mapping before wiring the driver.
- [ ] **4.2 ESP-NOW receiver** — register RX callback, parse incoming `deck_report_t` structs (see `../05-integration`), route by `node_id`.
  - Test: dummy sender (or Module 1/2 node) delivers a packet, hub logs it and routes to the right in-memory table.
- [ ] **4.3 UART receiver (2nd hardware UART)** — parse Arduino's line-based CSV reports.
  - Test: feed the CSV format by hand over serial monitor, confirm correct parse.
- [ ] **4.4 Aggregation store** — in-memory table (or SD-card rolling summary log if an SD module gets added): per-node counts, unique-entity totals, flagged-tracker alerts.
- [ ] **4.5 (Optional) WiFi AP + local web dashboard** — separate radio use, doesn't conflict with sniffer node's promiscuous mode since it's a different device.

## Standalone-complete checklist
- [ ] Dashboard fully navigable on synthetic data (do this BEFORE any node is wired in)
- [ ] ESP-NOW callback parses a real/dummy `deck_report_t` correctly
- [ ] UART parser handles the CSV protocol correctly
- Swapping synthetic data for live node data is the last step — see Build Order phase 4 in `../docs/opsec-osint-deck-plan.md`.
