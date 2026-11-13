# HUB migration plan — move the phone-driven orchestrator onto the ESP32-DIV hardware

**Status: PLAN ONLY (specced 2026-11-12, not started). Do not implement until the user says go.**

## Goal

Retire the overlap between the HUB (stock ESP32-DIV firmware, button/touch-driven) and the
WIFI-NODE orchestrator (our firmware + phone app). Move **our orchestrator firmware onto the HUB
hardware** and make it **feature-complete**, so one phone-driven board does everything:

- Keep: WiFi recon/attacks, BLE scan/spam, HID trap, attack targeting (everything built in `orchestrator/v1`).
- Gain from the hardware: **external U.FL antenna** (range/sensitivity), **on-board GPS**, **SD card**.
- Add to our firmware: **CC1101 sub-GHz** and **3× NRF24** (2.4 GHz) modes — the radio-shield
  features that today only exist in stock ESP32-DIV.
- Drop: the PCF8574 buttons (navigate/launch from the phone). The 2.4" touch TFT becomes an
  optional on-device status screen; this can let us retire the UNO READOUT.

Net result: **HUB = the one phone-driven power tool.** The C3 (today's orchestrator brain) and the
UNO READOUT are freed for other roles (**the freed-C3 role is "decide later"** per 2026-11-12).

This forks `docs/hardware-layout.md` (HUB firmware would change from "stock ESP32-DIV v1.7.2" to
"our orchestrator, ported"). Update that doc + `docs/orchestration-plan.md` when implemented.

## Target hardware (HUB)

Classic **ESP32-WROOM-32U** (U.FL) + 2.4" ILI9341 touch/SD TFT + NEO-7M GPS + radio shield
(3× NRF24L01+ + CC1101 @433 MHz). Pin map is already in `docs/hardware-layout.md` → "HUB pins".
Key pins repeated here for the port:

| Function | GPIO |
|---|---|
| TFT ILI9341 CS/RST/DC | 15 / 0 / 2 |
| TFT MOSI/SCK/MISO | 13 / 14 / 12 |
| TFT backlight (PWM) | 32 |
| Touch XPT2046 CS | 33 |
| SD CS | 5 |
| Touch+SD shared bus CLK/MOSI/MISO | 18 / 23 / 19 |
| GPS NEO-7M TX/RX (ESP32 RX/TX) | 3 / 1 (USB-serial pins — unplug to flash) |
| NRF24 #1 CE/CSN | 4 / 5 (CSN shares SD_CS) |
| NRF24 #2 CE/CSN | 26 / 27 |
| NRF24 #3 CE/CSN | 16 / 17 |
| CC1101 CS/GDO0/GDO2 | 27 / 26 / 16 (reuses NRF24 #2/#3 pins) |
| Radios shared SPI SCK/MOSI/MISO | 18 / 23 / 19 (shared with SD + touch) |

## Two constraints that shape the whole design

1. **One ESP32 radio (2.4 GHz).** WiFi promiscuous/inject vs our BLE control vs HID are still
   mutually exclusive — the existing role-handoff (`roleOf` / `requestMode`) carries over unchanged.
2. **The external radios are separate SPI silicon**, which is the good news:
   - **CC1101 is sub-GHz (~433 MHz)** — fully independent of the ESP32's 2.4 GHz radio. Sub-GHz
     modes can run **while BLE control stays up → the phone stays connected** (like the BLE scanners).
   - **NRF24 is 2.4 GHz but separate silicon** — electrically independent of the ESP32 radio; only a
     physical-RF coexistence concern (separate antennas, keep them apart). Likely also runs with the
     phone connected, with the ESP32 BLE at low duty. **Verify on the bench.**
   - **The real mutual-exclusion for the radios is the shared SPI bus**: SD + touch + 3× NRF24 +
     CC1101 all sit on SCK18/MOSI23/MISO19, and **GPIO5 is both SD_CS and NRF24 #1 CSN**, and
     GPIO 26/27/16 are reused between NRF24 #2/#3 and CC1101. Only one SPI device may transact at a
     time, and we must follow cifertech's discipline: **reclaim the bus and re-mount the SD card
     after a radio tool runs.** This becomes an "SPI owner" arbiter in our firmware.

**Radio-role model extension:** add a CONTROL-class treatment for NRF24/CC1101 modes (phone stays
connected, SPI-arbitrated), distinct from the ESP32-radio WIFI role that drops BLE. So sub-GHz / NRF
modes should feel like the BLE scanners (live, connected), not like the WiFi attacks (handoff drop).

## Firmware port — C3 → classic ESP32 (the easy part)

Same Arduino core (esp32 3.3.12) and same APIs (`esp_wifi`, `esp_wifi_80211_tx`, NimBLE,
`esp_http_server`, lwip). Expect a **pin + display + flash-budget** job, not a rewrite:

- **Pins:** replace C3 constants — `LINK_TX/RX` (21/20), `BOOT_BTN` (9), LED pins. On classic ESP32
  the BOOT strap is GPIO0 (also TFT RST here — conflict, so the physical escape button needs its own
  free GPIO or we rely on the phone/touch). Remap `Serial1` if we keep the UNO link.
- **Display decision (pick during Phase 3):**
  - (a) Drive the 2.4" ILI9341 directly with TFT_eSPI (retires the UNO) — new display code, but one
    fewer board. *Recommended end state.*
  - (b) Keep headless + phone-only for the first port (fastest).
  - (c) Keep the UNO READOUT over UART (works today, but wastes the HUB's own screen).
- **BLE on classic ESP32:** NimBLE works and coexistence is *better* than the C3, but the image is
  **bigger** (BT classic controller present). Plus RF24 + CC1101 + TinyGPS++ + SD/FS + TFT_eSPI libs.
  **Flash budget risk on a 4 MB board** — likely need the `huge_app` (3 MB app, no OTA/SPIFFS)
  partition. That's fine because storage goes to the SD card, not SPIFFS. **Verify early.**
- **Antenna:** the U.FL/RP-SMA pigtail is a straight gain for all WiFi RX/TX — no code.

## New capabilities to add (the real work)

### SD card — the constraint-dissolver (do this first after the port)
Removes the whole "no SD / phone is the disk" design. On the shared SPI bus with an SD_CS/NRF arbiter:
- **Handshake capture → real `.pcap` on SD** (finally; the pcap-to-phone milestone becomes pcap-to-SD).
- **WiGLE CSV** for wardriving, scan logs, evil-portal hit logs.
- Optional: expose files to the phone (list/download over a BLE DATA transfer, or just pull the card).

### GPS (NEO-7M) + on-device wardriving
- NMEA parse (TinyGPS++). Stamp every AP/BLE sighting with lat/lon/time → WiGLE CSV on SD.
- Drops the "phone GPS + timestamp-join" workaround. GPS is on the USB-serial pins — **unplug to flash.**

### CC1101 sub-GHz (433 MHz) — runs with phone connected
- Modes: sub-GHz **capture / replay / jam**, frequency/RSSI scan. Lib: SmartRC-CC1101-Driver-Lib
  (from the ESP32-DIV `Libraries/` zip) or raw SPI.
- New phone opcodes + app mode group "Sub-GHz" + detail pages. SPI-arbitrated; own-lab only.

### NRF24 ×3 (2.4 GHz)
- Modes: 2.4 GHz **scanner/analyzer**, **jammer**, **ESB / MouseJack** (wireless keyboard/mouse
  sniff-spoof). Lib: RF24 + nrf24 scanner routines (reference cifertech's implementation).
- 3V3 only; 10 µF + 100 nF across each module. Separate antennas, kept apart (near-field interference
  bit the earlier build). New opcodes + app mode group "2.4 GHz / NRF".

## Phone app changes
- New mode groups: **Sub-GHz** and **2.4 GHz / NRF**, with the same full-screen "what it does / how it
  works / Run" detail pages and the targeting pattern where it applies.
- GPS/wardrive UI (fix status, log start/stop, file list).
- Mark sub-GHz / NRF modes as "phone stays connected" (CONTROL-class), unlike the WiFi handoff modes.
- Keep attack targeting + the own-lab warnings; extend targeting to sub-GHz replay (capture-then-replay
  is inherently targeted).

## Opcode map (to extend — reserve ranges now)
- 0x00–0x07 WiFi, 0x10–0x15 BLE/HID, 0x20–0x23 targeting (existing).
- **0x30–0x3F → CC1101 sub-GHz.** **0x40–0x4F → NRF24.** **0x50–0x5F → GPS/wardrive/SD.** (Draft.)

## Rollout phases (each independently compile-verifiable)
0. **Port core** to classic ESP32 — WiFi/BLE/HID working on the HUB with the external antenna, headless/phone. Prove the flash budget + partition here.
1. **SD card** — pcap + WiGLE CSV + logs. (Biggest payoff; unblocks handshake pcap.)
2. **GPS + wardriving** — NMEA parse, stamped CSV to SD.
3. **On-device TFT** (TFT_eSPI) — optional; retires the UNO.
4. **CC1101 sub-GHz** modes + app UI.
5. **NRF24** modes + app UI.
6. **App polish** — file browser, wardrive dashboard, group layout.

## Risks / open questions (resolve before/early in implementation)
- **Flash budget** on the 4 MB WROOM: NimBLE (classic) + RF24 + CC1101 + GPS + SD + TFT. Measure after
  Phase 0; expect `huge_app` partition. If it won't fit, consider dropping on-device TFT (stay phone-only).
- **SPI contention**: the SD_CS = NRF24#1_CSN overlap and the NRF/CC1101 pin reuse need a strict SPI
  owner/arbiter + SD re-mount after radio ops. This is the fiddliest part — budget bench time.
- **2.4 GHz coexistence**: NRF24 (2.4) alongside the ESP32's BLE — verify the phone link survives NRF ops; separate, spaced antennas.
- **Verify the board**: is the WROOM-32U a full dev board (USB + headers)? Confirm the radio-shield pin
  table against cifertech's `Schematic/v1/ESP32DIV-SHIELD.jpg` before soldering.
- **Flashing**: GPS on GPIO1/3 — unplug to flash; `erase-flash` first; esptool for classic ESP32.
- **Licensing/reuse**: ESP32-DIV is open — we may reference (not necessarily copy) its NRF24/CC1101
  routines. Keep our own structure.

## What happens to the other boards
- **C3 #1** (today's orchestrator brain) → freed → **role TBD** (user chose "decide later" 2026-11-12).
- **UNO + 3.5"** → freed if the HUB drives its own TFT; otherwise it stays as the HUB's display (Phase-3 decision).
- **STATUS watch**, **BLE-SCAN spare** → unchanged for now.
