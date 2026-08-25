# Component 3 — Sub-GHz Node + Control Surface

**Device:** Arduino Uno + big LCD
**Status:** 🟨 `firmware/subghz_control/subghz_control.ino` (steps 3.1-3.3, 3.5-3.6; 3.4 LCD render stubbed pending part decision) compiles clean against `arduino:avr:uno`, not yet flashed/tested on hardware

Two jobs on the one radio-less device: passive 433/315MHz listening, and the deck's physical control panel/status board.

## Hardware
- Sub-GHz receiver: RXB6 / XY-MK-5V superheterodyne OOK/ASK module (avoid cheap regen receivers — noisy).
- Antenna: ~17.3cm wire, quarter-wave for 433MHz.
- Control input: rotary encoder or 3–4 momentary buttons on remaining digital pins.
- Big LCD: confirm part — HD44780 16x2/20x4 character LCD (get I2C backpack if you don't have one, frees up pins) vs graphic TFT (Uno only has 2KB SRAM — a full-frame-buffer color TFT is tight; character LCD or page-buffered graphic LCD is safer).

## Build in this order

- [ ] **3.1 Sub-GHz receive via rc-switch** — decode common fixed-code OOK protocols (garage remotes, cheap doorbells, some weather stations).
  - Test: reliably decode a known remote (garage door opener or cheap 433MHz doorbell) at your desk.
- [ ] **3.2 Raw pulse-timing fallback** — for anything rc-switch doesn't recognize, capture raw pulse widths on an interrupt pin and log them.
  - Test: log shows pulse-width data for an unrecognized signal source.
- [ ] **3.3 Log format** — `{ ts, protocol_or_raw, code, pulse_length, repeat_count }`.
- [ ] **3.4 Big LCD driver + init** — get exact wiring once part is confirmed.
  - Test: LCD renders and updates without visible tearing/flicker.
- [ ] **3.5 Control input (encoder/buttons)** — wire to remaining digital pins.
  - Test: navigate a basic 2–3 screen menu (idle/status, menu, detail-view) using only the physical controls.
- [ ] **3.6 State machine** — idle/status screen (sub-GHz hit count) → menu mode → detail view of last captured signal.

## Standalone-complete checklist
- [ ] rc-switch decode confirmed on a real remote
- [ ] Raw fallback capture confirmed
- [ ] LCD + menu navigation solid
- Serial link to hub (UART, line-based CSV) comes later — see `../05-integration`.
