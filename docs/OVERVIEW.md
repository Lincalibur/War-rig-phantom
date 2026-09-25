# CyberDeck v2 — overview

Start here. This covers what the deck does, the hardware decisions and why they were made, and where everything else lives.

| Need | Go to |
|---|---|
| Exact parts list and every pin connection (KiCad input) | [`PCB-BOM-AND-NETLIST.md`](PCB-BOM-AND-NETLIST.md) (authoritative) |
| Interactive build page: BOM checklist, board layout, searchable pin tables | [`esp32-hub-board.html`](esp32-hub-board.html) (also deployed to GitHub Pages) |
| KiCad project | [`../KiCad Design/`](../KiCad%20Design/) |
| Firmware, build settings, bring-up order | [`../v2/README.md`](../v2/README.md) |
| Remote flashing station | [`debian-esp32-server-guide.md`](debian-esp32-server-guide.md) |

The v1 ESP-NOW build (C3 nodes, ESP32-1732S019 hub) was removed from the tree. It is kept in git under the tag `v1-espnow-archive`.

## Goal
A portable wardriving deck that scans and logs the radio environment around you: WiFi networks and devices, BLE devices and sub-GHz signals. Every sighting is GPS-tagged. The case is about **10 x 15 cm**.

**Everything here is passive listening.** Don't add deauth, injection or jamming features.

## Architecture

```
                 GPS TX (NEO-7M) ------+----------------+
                                       |                |
                            +----------v-----+  +-------v--------+
                            | ESP32-C3 PRO   |  | ESP32-H2       |
                            | Mini: WiFi     |  | SuperMini: BLE |
                            | 2.4 GHz        |  | (+802.15.4)    |
                            +-------+--------+  +-------+--------+
                                    | UART              | UART   (wired, 3.3 V)
                            +-------v-------------------v--------+
                            |  ESP32-S3-DevKitC-1 N16R8 (hub)    |
                            |  SPI: 2.8" ILI9341, microSD,       |
                            |       CC1101 433 MHz               |
                            |  5-way nav, VBAT sense on GPIO6    |
                            +------------------------------------+
  18650 -> IP5310 board -> SW_PWR -> 5V_RAIL -> each module (via its own Schottky)
```

The key idea is that **nodes never transmit**. They are receive-only scanners that report over wired UART. That removes the ESP-NOW timing problems and the near-field desense that broke v1. Each node reads the GPS directly, so it tags its own sightings with position and time.

## Decisions

| Date | Decision | Why |
|---|---|---|
| 2026-09-20 | Fresh start as one KiCad PCB | The ESP-NOW build never worked reliably with both nodes active |
| 2026-09-20 | Wired UART instead of ESP-NOW | Nodes stay receive-only, so there's no timing or near-field interference |
| 2026-09-20 | ESP32-S3 hub with a separate 2.8" SPI TFT | More GPIO and RAM than the ESP32-1732S019, plus a bigger screen |
| 2026-09-20 | GPS fanned out to both nodes | Every sighting carries its own location |
| 2026-09-20 | CC1101 on the hub SPI bus | Replaces the faulty RXB6 sub-GHz receiver |
| 2026-09-20 | Ready-made power module, no custom charge/boost stage | Power is the part most likely to cause random resets |
| 2026-09-20 | WiGLE-format CSV on microSD | Uploadable to wigle.net |
| 2026-09-25 | Source everything from nde3d.co.za | One South African supplier. This forced the substitutions below |

**nde3d substitutions (2026-09-25).** Full detail is in section 1.4 of `PCB-BOM-AND-NETLIST.md`.
- WiFi node: ESP32-C3 PRO Mini replaces the XIAO C5. That means **no 5 GHz**; most dual-band APs still beacon on 2.4 GHz.
- BLE node: ESP32-H2 SuperMini replaces the XIAO C6. If H2 BLE misbehaves in Arduino, fall back to a C3 SuperMini.
- Power: an IP5310 power-bank board plus an 18650 replaces the PowerBoost 1000C and LiPo. The IP5310 has no EN or LBO pin, so the power switch sits on the 5 V output and low battery comes from a VBAT divider on GPIO6.
- Buttons: a 5-way nav module replaces five tactile switches.
- GPS: NEO-7M. CC1101: 433 MHz for South Africa.

## What the radios can and can't do
- **WiFi (C3):** promiscuous mode captures management frames and data-frame headers. That covers APs (SSID, BSSID, channel, encryption, vendor, hidden SSIDs, RSSI), clients (MAC, probed SSIDs, random-MAC detection) and client-to-AP links. It listens on one channel at a time and hops. It's 2.4 GHz only.
- **BLE (H2):** advertisement scanning gives MAC, RSSI, name, manufacturer ID and tracker detection (AirTag / Find My / Tile). It can't do Classic Bluetooth. 802.15.4 (Zigbee/Thread) is possible later.
- **Sub-GHz (CC1101, 433 MHz):** OOK/ASK/FSK reception. Not implemented in firmware yet.
- **Not possible here:** true wideband spectrum analysis, LoRa decoding, ADS-B or cellular. Those need an RTL-SDR, which sits outside this board.

## Board
- **2 layers with a solid ground plane** on the bottom, never split. Everything is socketed: every module plugs into female headers so it can be swapped and reflashed.
- **Placement:** the hub and display go on the top face. The nodes, CC1101, IP5310 and battery go on the bottom face.
- **RF placement:** the GPS goes on the top edge with no copper under its patch. The WiFi and BLE nodes go at opposite short edges, with antenna keep-outs running to the board edge. The CC1101 antenna goes at an edge away from the nodes.
- **Power traces:** 5V_RAIL at least 1 mm wide, and at least 1.5 mm from the IP5310 to C1. Place each bulk cap right at the pin it feeds.
- **USB-C access:** every module's USB-C port must be reachable from the case edge for flashing.
- **Board size is not settled yet.** `PCB-BOM-AND-NETLIST.md` says 100 x 150 mm (portrait). The layout concept in `esp32-hub-board.html` is drawn at 170 x 100 mm. Pick one before routing.

## Power budget
- **5 V:** about 900 mA to 1 A peak (WiFi node ~250–300 mA, BLE node ~150 mA, S3 plus its 3V3 loads ~300–350 mA, display ~100 mA). The IP5310 board is rated 3.1 A. **The mini slide switch is the weak point:** many are rated only 0.3–0.5 A, so buy one rated 1 A or more.
- **3.3 V:** the S3 devkit's onboard LDO (~800 mA) feeds the display, GPS, CC1101 and SD, which puts it close to the limit at peak. If the hub browns out, give the display and SD their own 3.3 V regulator.
- Bench-test the IP5310 for charge-while-running (a possible brief dropout) and for idle auto-shutdown.

## Status (2026-09-25)
- **PCB:** the BOM and netlist are done. Items marked [VERIFY] (node pinouts, CC1101/GPS/TFT/nav pin order, devkit row spacing) must be checked against the physical parts. The KiCad schematic is in progress.
- **Firmware** (`../v2/`): written but **never compiled or flashed**. It still targets the XIAO C5/C6 pin labels and must be remapped to the C3 PRO Mini and H2 SuperMini. Not implemented yet: CC1101 reception, 802.15.4, and reading VBAT on GPIO6.

## Next steps
1. With the parts in hand, verify every [VERIFY] pinout and footprint. Settle the H2 node GPIOs and the board size.
2. Finish the KiCad schematic, then place and route.
3. Get the firmware compiling (Debian server or local `arduino-cli`) and remap the node pins.
4. Bench bring-up on loose modules *before* ordering the PCB: hub, then each node, then GPS, then a walk test and a WiGLE upload.
5. Order the PCB.
