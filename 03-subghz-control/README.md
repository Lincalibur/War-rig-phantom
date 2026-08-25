# Component 3 — Sub-GHz Node

**Device:** ESP32-C3, no display
**Status:** 🟨 `firmware/subghz_node/subghz_node.ino` (steps 3.1-3.3) compiles clean against `esp32:esp32:esp32c3:CDCOnBoot=cdc`, not yet flashed/tested on hardware

**ESP-NOW-consolidated redesign:** this component used to be an Arduino Uno driving a big LCD control surface (rc-switch decode, raw fallback, and a local menu/state-machine UI — steps 3.4-3.6). That design is retired; the old sketch is still in git history / on `main` if needed. Sub-GHz sensing now lives on a third ESP32-C3 node, alongside the WiFi sniffer and BLE hunter, reporting over ESP-NOW as `deck_report_t` (node_id=3). All display/menu/control responsibilities moved to the hub console (Component 4) — this node has no screen and no buttons.

Passive 433/315MHz listening only.

## Hardware
- Sub-GHz receiver: RXB6 / XY-MK-5V superheterodyne OOK/ASK module (avoid cheap regen receivers — noisy).
- Antenna: ~17.3cm wire, quarter-wave for 433MHz, on the RX module's antenna pad. (External/upgraded antenna is a later pass, not blocking this build — see [[cyberdeck_power_supply_requirements]]/COMPONENTS.md notes on U.FL boards.)
- Board: ESP32-C3 Dev Module. Any two free GPIOs work for `RCSWITCH_PIN`/`RAW_PIN` — unlike the Uno, all C3 GPIOs support external interrupts, so there's no fixed INT0/INT1 constraint.

## Build in this order

- [ ] **3.1 Sub-GHz receive via rc-switch** — decode common fixed-code OOK protocols (garage remotes, cheap doorbells, some weather stations).
  - Test: reliably decode a known remote (garage door opener or cheap 433MHz doorbell) at your desk.
- [ ] **3.2 Raw pulse-timing fallback** — for anything rc-switch doesn't recognize, capture raw pulse widths on an interrupt pin and log them.
  - Test: log shows pulse-width data for an unrecognized signal source.
- [ ] **3.3 Log format** — `{ ts, protocol_or_raw, code, pulse_length, repeat_count }`, printed to Serial and sent to the hub over ESP-NOW.

## Standalone-complete checklist
- [ ] rc-switch decode confirmed on a real remote
- [ ] Raw fallback capture confirmed
- ESP-NOW link to hub (broadcast, `deck_report_t` node_id=3) comes together with the other two nodes in `../05-integration`.
