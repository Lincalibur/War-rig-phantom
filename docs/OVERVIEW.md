# CyberDeck — overview

Start here. This page covers what the deck does, the hardware decisions and the reasons for them, and where everything else lives.

> **Current build: SPECTRE handheld + passive modules.** The kit is separate modules, each with its own screen, on one 18650 pack. SPECTRE (the `HUB`) is an ESP32-WROOM-32U running the **ported CyberDeck orchestrator** — *not* stock cifertech ESP32-DIV firmware (that was only a reference example). It has a **3.5" ILI9488 8-bit parallel TFT**, a NEO-7M GPS, and uses the ESP32's built-in 2.4 GHz radio. Driven from the `CONSOLE` phone app over BLE. The hardware-verified state is in [`../spectre/v1/STATUS.md`](../spectre/v1/STATUS.md).
>
> **On the "full capability" note:** SPECTRE runs the complete orchestrator toolkit including active/transmitting WiFi + BLE tools, for **authorized testing against the project's own test laptop and spare router only**. The earlier plan for a 3× NRF24L01 + CC1101 radio shield did **not** happen — the 8-bit parallel TFT leaves room for only one expansion SPI radio, deferred. See the Decisions table and "What the radios can and can't do".

| Need | Go to |
|---|---|
| **Current build: boards, SPECTRE pins, power, case** | [`hardware-layout.md`](hardware-layout.md) |
| Build page: shopping list, wiring diagram, build order | [`build-page.html`](build-page.html), deployed to <https://lincalibur.github.io/War-rig-phantom/> |
| Board names (`SPECTRE`, `SENTINEL`, `BEACON`, `CONSOLE`, `RELAY`) | [`hardware-layout.md`](hardware-layout.md#module-names) |
| SENTINEL C3 → UNO wiring | [`proto-readout-wiring.html`](proto-readout-wiring.html) |
| SPECTRE firmware (orchestrator on one WROOM) | [`../spectre/v1/`](../spectre/v1/), status in [`../spectre/v1/STATUS.md`](../spectre/v1/STATUS.md) |
| SENTINEL firmware (passive watcher, C3 + UNO) | [`../sentinel/v1/`](../sentinel/v1/) |
| `BLE-SCAN` board spec (on hand, not in this build) | [`BOARD-BLE-SCAN-ESP32-1732S019.md`](BOARD-BLE-SCAN-ESP32-1732S019.md) |
| Shelved designs: single PCB, S3 hub, Pi 5 clamshell | Removed from the tree 2026-11-20; recoverable from git tag `pre-cleanup-2026-11-20` and history |

The v1 ESP-NOW build (C3 nodes, ESP32-1732S019 hub) is kept in git under the tag `v1-espnow-archive`.

## Goal
A portable wireless multi-tool and recon kit that scans and logs the radio environment around you, flags devices that keep following you, and — on SPECTRE — exercises a full Ghost-ESP-style toolkit for hands-on learning. It covers WiFi and BLE on the ESP32's built-in 2.4 GHz radio, with GPS-tagged output. Later the logs go to a laptop, to analyse and learn from what was found. (A sub-GHz CC1101 or a 2.4 GHz NRF24 is a possible future add-on via SPECTRE's one expansion SPI slot.)

**SPECTRE's active tools transmit. Authorized use only.** The deauth, flood, evil-portal, BLE-spam and adv-flood tools are for **your own test laptop and spare router on an isolated bench** — never networks, devices or people you don't own. The standalone recon modules (`SENTINEL`, `BEACON`) stay passive, listening and logging only.

## Architecture

```
  SPECTRE  ESP32-WROOM-32U + router antenna (U.FL)      ported CyberDeck orchestrator
           3.5" ILI9488 8-bit parallel TFT (320x480), NEO-7M GPS on UART2
           phone/BLE control (no buttons, no touch, no SD)
           WiFi scan/deauth/evil-portal/packet-monitor/wardrive, BLE scan/spam,
           handshake -> .pcap streamed to the phone, HID   (built-in 2.4 GHz radio)
           one expansion SPI slot reserved for a future CC1101/NRF24 (unused)

  SENTINEL  ESP32-C3, WiFi/BLE promiscuous --19200 baud UART--> UNO + 3.5" TFT  (passive, never TX)

  BEACON  ESP32-C3 + OLED watch: GPS + WiFi CSV to an Android phone; docks to the kit now and then

  CONSOLE  Android (Flutter) app: drives SPECTRE over BLE; receives the .pcap

  3x 18650 (1S3P) -> master switch -> 5V 2A charge/boost module -> 5 V to every module
```

Each module works on its own. The passive modules (SENTINEL, BEACON) never transmit; SPECTRE transmits only in its active tools, own-lab only.

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
| 2026-11-02 | Not the CYD (ESP32-2432S028R) | Its pins are shared, so the GPS and other peripherals can't be added cleanly |
| 2026-11-13 | Drop the PCF8574 button matrix; drive the HUB from the CONSOLE phone app (BLE) and the touchscreen | Fewer parts and wires, no I2C expander, and the app is the main control surface anyway. GPIO 21/22 freed. The GPS wires straight to the ESP32, so the protoboard goes too |
| 2026-11-02 | 2.4" ILI9341 instead of the 2.8" | R299 against R468, and the firmware runs on both |
| 2026-11-02 | 3x 18650 in parallel on a single-cell 5V 2A module | One pack powers the whole kit for about 6 hours. A 1S pack avoids the 2S charger problem |
| 2026-11-02 | `WIFI-NODE` + `READOUT` and `STATUS` stay as standalone modules | They already work. Each module has its own job and screen |
| 2026-11-10 | ~~Enable the full ESP32-DIV toolkit on the HUB, incl. active tools, and add a 3× NRF24 + CC1101 radio shield~~ | Wanted the complete feature set, not just passive. **Partly superseded (2026-11-14):** the toolkit happened via our own orchestrator, but the 3× NRF24 + CC1101 shield did not — the parallel TFT leaves one expansion SPI slot, so at most one future radio |
| 2026-11-10 | Repositioned from "passive only" to "authorized-testing multi-tool" | SPECTRE transmits now. Docs/branding updated with an own-lab ethics note; the recon modules stay passive |
| 2026-11-14 | **Run our own ported orchestrator on the HUB (SPECTRE), not stock cifertech ESP32-DIV firmware** | We already had the orchestrator (C3 + UNO); porting it onto one WROOM gives full control, the phone-app/BLE workflow, and avoids being boxed in by stock firmware. cifertech ESP32-DIV stays a reference only. Display moved to a 3.5" ILI9488 8-bit parallel panel (verified on hardware) |

## What the radios can and can't do
- **SPECTRE (ESP32-WROOM-32U + external antenna):** the orchestrator toolkit on the **built-in 2.4 GHz radio only**. WiFi scanning (OUI vendor + encryption type), packet-monitor waterfall across channels, deauth detection **and** the active WiFi tools (deauth with an app-chosen target, beacon/probe/karma flood, captive/evil portal); BLE scanning/sniffing plus BLE spam and a best-effort adv-flood "jammer"; WPA handshake capture streamed to the phone as `.pcap`; live GPS-stamped wardrive (on-screen, no logging); HID. No SD — captures go to the phone over BLE.
- **Expansion radio (future, unused):** SPECTRE's carrier has **one** SPI slot that could take a single CC1101 (sub-GHz, e.g. 433 MHz capture/replay) or a single NRF24L01 (2.4 GHz). The 8-bit parallel TFT uses too many GPIOs for the old 3× NRF24 + CC1101 shield — see [`hardware-layout.md`](hardware-layout.md#radios--expansion). Not wired or in firmware yet.
- **SENTINEL (C3, promiscuous):** APs, clients, probed SSIDs and deauth counts while channel hopping. Clients are sorted by persistence, so the device shadowing longest is the "following me" candidate. Passive — never transmits.
- **Not covered:** 5 GHz WiFi, Classic Bluetooth, and a true wideband SDR (no RTL-SDR in this build). A CC1101, if added, is fixed-band, not a spectrum analyser.
- **Not possible here:** cellular, including fake-tower (IMSI catcher) detection. That needs dedicated hardware.

## Status (2026-11-21)
The live, cross-machine status is [`../spectre/v1/STATUS.md`](../spectre/v1/STATUS.md) — this is a summary.
- **SPECTRE:** firmware done and compiles (~34% flash / 22% RAM). Display **solved on hardware** (ILI9488 3.5" parallel, 320×480). Pins remapped 2026-11-15 to the SPECTRE-carrier layout. Modes are hardware-untested — driven from the CONSOLE app over BLE.
- **SENTINEL:** firmware compile-verified (`sentinel/v1/`); the C3 + UNO pair repurposed from the old WIFI-NODE/READOUT.
- **CONSOLE:** Android APK built (`CyberDeck.apk`).
- **PCB:** a SPECTRE carrier board is on branch `spectre-pcb`, hand-routed, fab files generated, hardware-untested.
- **Case:** the Pi 5 layout and filler no longer fit; the layout will be redrawn once the parts are measured.

## Next steps
1. Hardware-test SPECTRE's modes over BLE from the app — passive first (WiFi/BLE scan, GPS/satellite, wardrive, packet monitor), then handshake → `.pcap` pull-to-phone, then the active-TX modes.
2. Run the active-TX tools **only** on the authorized bench: the spare router + test laptop on an isolated network, nothing else nearby.
3. Hardware-test SENTINEL on the C3 + UNO pair.
4. Optional/deferred: add one expansion radio (a CC1101 at 433 MHz, or a single NRF24) and wire the R1/IO4 boot resistors first.
5. Measure the parts and lay out the case.
6. Plan the laptop side: collect SPECTRE's `.pcap` captures and BEACON's CSVs for analysis.
