# CyberDeck — overview

Start here. This page covers what the deck does, the hardware decisions and the reasons for them, and where everything else lives.

> **Current build (2026-11-02): ESP32-DIV v1 handheld.** The Pi 5 hub was dropped as overkill. The kit is now separate passive modules, each with its own screen, on one 18650 pack. The parts are listed under Architecture. The parts were ordered from nde3d on 2026-11-02.

| Need | Go to |
|---|---|
| **Current build: boards, HUB pins, power, case** | [`hardware-layout.md`](hardware-layout.md) |
| Build page: shopping list, wiring diagram, build order | [`build-page.html`](build-page.html), deployed to <https://lincalibur.github.io/War-rig-phantom/> |
| Board names (`HUB`, `WIFI-NODE`, `READOUT`, `STATUS`, `BLE-SCAN`) | [`hardware-layout.md`](hardware-layout.md#board-names) |
| `WIFI-NODE` → `READOUT` wiring | [`proto-readout-wiring.html`](proto-readout-wiring.html) |
| Firmware for the C3 and UNO boards | [`../v2/README.md`](../v2/README.md) |
| `BLE-SCAN` board spec (on hand, not in this build) | [`BOARD-BLE-SCAN-ESP32-1732S019.md`](BOARD-BLE-SCAN-ESP32-1732S019.md) |
| Shelved designs: single PCB, S3 hub, Pi 5 clamshell | [`PCB-BOM-AND-NETLIST.md`](PCB-BOM-AND-NETLIST.md), [`esp32-hub-board.html`](esp32-hub-board.html), [`../cad/case_filler/`](../cad/case_filler/), git history |

The v1 ESP-NOW build (C3 nodes, ESP32-1732S019 hub) is kept in git under the tag `v1-espnow-archive`.

## Goal
A portable counter-surveillance kit that scans and logs the radio environment around you and flags devices that keep following you. It covers WiFi networks and devices, and BLE devices and trackers, with GPS-tagged logs. Later the logs go to a laptop, to analyse and learn from what was found.

**Everything here is passive listening.** Don't add deauth, injection or jamming features. The ESP32-DIV firmware ships with some of those. They are not used, and the build has no NRF24 or CC1101 radios for them.

## Architecture

```
  HUB  ESP32-WROOM-32U + router antenna (U.FL)          ESP32-DIV v1 firmware
       2.4" ILI9341 touch + SD, PCF8574 buttons, NEO-7M GPS
       WiFi scan, BLE scan, deauth detector, packet monitor, GPS wardriving -> SD (WiGLE CSV)

  WIFI-NODE  ESP32-C3, WiFi promiscuous --19200 baud UART--> READOUT  UNO + 3.5" TFT

  STATUS  ESP32-C3 + OLED watch: GPS + WiFi CSV to an Android phone; docks to the kit now and then

  3x 18650 (1S3P) -> master switch -> 5V 2A charge/boost module -> 5 V to every module
```

Each module works on its own. None of them transmit on the scanned bands.

## Decisions

| Date | Decision | Why |
|---|---|---|
| 2026-09-20 | Wired, receive-only nodes instead of ESP-NOW | The ESP-NOW build never worked reliably with both nodes active, because of timing and near-field interference |
| 2026-09-20 | WiGLE-format CSV logs | Uploadable to wigle.net |
| 2026-09-25 | Source everything from nde3d.co.za | Nearest shop, one supplier |
| 2026-10-05 | Reposition as a counter-surveillance tool | "Am I being followed" detection on top of the same passive radios |
| 2026-10-09 | 22 × 22 cm clamshell, one screen per module, no custom PCB | Bigger case on hand. Modules and screens already owned |
| 2026-10-31 | Raspberry Pi 5 4GB hub and 7" panel | Superseded on 2026-11-02 |
| 2026-11-02 | Drop the CC1101 for an RTL-SDR | Superseded the same day when the Pi was dropped |
| 2026-11-02 | `STATUS` becomes a removable watch | It already logs GPS and WiFi scans to a phone |
| 2026-11-02 | **Drop the Pi 5. The HUB is an ESP32-DIV v1-style handheld on an ESP32-WROOM-32U** | The Pi was overkill. The ESP32-DIV gives a working scanner and wardriver UI for about R900, at low power, with external-antenna range. Losing Kismet, the RTL-SDR and the 7" panel is accepted |
| 2026-11-02 | v1 target (classic ESP32), not v2 (S3) | nde3d has no WROOM-1U S3 board. S3 boards with octal PSRAM lose GPIO 35-37, which v2 uses for the TFT |
| 2026-11-02 | Not the CYD (ESP32-2432S028R) | Its pins are shared, so the GPS and buttons can't be added cleanly |
| 2026-11-02 | 2.4" ILI9341 instead of the 2.8" | R299 against R468, and the firmware runs on both |
| 2026-11-02 | 3x 18650 in parallel on a single-cell 5V 2A module | One pack powers the whole kit for about 6 hours. A 1S pack avoids the 2S charger problem |
| 2026-11-02 | `WIFI-NODE` + `READOUT` and `STATUS` stay as standalone modules | They already work. Each module has its own job and screen |

## What the radios can and can't do
- **HUB (ESP32 + external antenna):** WiFi network scanning, a packet monitor across channels 1-14 with optional PCAP to SD, deauth detection, BLE scanning and sniffing (MAC, RSSI, packet count, last seen), and GPS wardriving that logs WiFi and BLE with position to SD as WiGLE CSV. It's 2.4 GHz only.
- **WIFI-NODE (C3, promiscuous):** APs, clients, probed SSIDs and deauth counts while channel hopping 1-13. Clients are sorted by persistence, so the device that has been shadowing longest is the "following me" candidate.
- **Not covered now:** 433 MHz and sub-GHz (no CC1101 or SDR), 5 GHz WiFi, Classic Bluetooth, and NRF24 2.4 GHz spectrum scanning (nde3d has no NRF24).
- **Not possible here:** cellular, including fake-tower (IMSI catcher) detection. That needs dedicated hardware.

## Status (2026-11-03)
- **Parts:** nde3d order placed on 2026-11-02 (R898.15), waiting to be collected at Strand.
- **HUB:** wiring drawn from the firmware pins. The prebuilt v1.7.0 firmware is downloaded and ready to flash. Nothing is wired yet.
- **WIFI-NODE + READOUT:** working on the bench.
- **STATUS:** working as a watch.
- **Case:** the Pi 5 layout and filler no longer fit. The layout will be redrawn once the parts are measured.

## Next steps
1. Collect the parts. Check that the ESP32 is a full dev board with USB and headers.
2. Flash the bare board with the ESP32-DIV v1.7.0 merged binary.
3. Wire the HUB one part at a time on USB power: TFT, touch, SD, PCF8574 and buttons, GPS, antenna. Run Touch Calibrate, then test the WiFi scan, BLE scan and GPS.
4. Bench-test the power system: matched cells, module output, runtime.
5. Measure the parts and lay out the case.
6. Plan the laptop side: collect the HUB SD logs and the STATUS CSVs for analysis.
