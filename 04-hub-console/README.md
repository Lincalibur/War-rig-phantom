# Component 4 — Hub Console

**Device:** Sunton/NDE3D ESP32-1732S019 (integrated 1.9" ST7789 IPS screen, non-touch, no buttons)
**Status:** 🟨 flashed and dashboard confirmed rendering (2026-09-03); wifi sniffer batch send fix just applied, not yet field-verified

Central aggregator: receives ESP-NOW reports from the two active C3 field nodes (wifi sniffer, wifi spectrum), renders one static dashboard.

**ESP-NOW-consolidated redesign:** the old Arduino Uno + big LCD control surface (former Component 3) is gone, along with its UART link into this hub (former step 4.3). Field nodes report over ESP-NOW broadcast, so this hub only needs one receive path.

**Sub-ghz (node_id=3) is intentionally not handled here right now** — that board's hardware is confirmed faulty and isn't in use. See the header comment in `hub_console.ino` for how to reintroduce it if replacement hardware arrives.

**Display:** since the board has no touch and no physical buttons, the hub renders everything — node health, networks, nearby devices, spectrum — on **one static dashboard** (`renderDashboard()`), redrawn every 3s. There is no screen switching or auto-cycling; nothing to navigate.

**Wire format:** the wifi sniffer sends `deck_wifi_batch_t` (up to 8 network/device entries in one packet, see `05-integration/shared/deck_report.h`), not a single-entry `deck_report_t` — the hub tells the two apart by exact packet size. The wifi spectrum node still uses plain `deck_report_t` for its per-sweep snapshot and heartbeats.

## Build order

- [x] **4.1 Dashboard renderer** — `renderDashboard()`: node-health strip, networks list (APs, sorted by RSSI, with enc/hidden/rogue-AP flags + vendor), nearby-devices list (probe-req senders, distinguished from networks via `DECK_WIFI_ENTRY_IS_DEVICE`), spectrum mini bar-graph.
- [x] **4.2 ESP-NOW receiver** — register RX callback, dispatch by exact packet size to `deck_wifi_batch_t` (wifi networks/devices) or `deck_report_t` (spectrum snapshot, heartbeats).
- [x] **4.4 Aggregation store** — in-memory tables (networks, devices) plus per-node health (last-seen, total reports).

## Standalone-complete checklist
- [x] Dashboard renders correctly with all tables empty (fresh boot, no nodes reporting yet) — node-health strip shows "never" for both active nodes
- [ ] ESP-NOW callback parses real `deck_wifi_batch_t`/`deck_report_t` packets correctly
- [ ] Live field test with wifi sniffer + wifi spectrum running — see `../README.md` verification steps.
