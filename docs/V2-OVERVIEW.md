# CyberDeck v2 — overview

Start here. This summarises the v2 redesign: what the hardware can and can't do, the decisions made, and where everything is. Details live in the linked files.

| Need | File |
|---|---|
| Exact parts list + every pin connection (for KiCad) | `PCB-BOM-AND-NETLIST.md` (authoritative) |
| Architecture, placement, power reasoning | `PCB-DESIGN.md` |
| Firmware, build settings, bring-up order | `../v2/README.md` |
| Old ESP-NOW build (superseded) | `PINOUTS.md`, `../01-*` … `../05-*` |

## Goal
A portable wardriving deck that scans and logs the radio environment around you — WiFi networks and devices, BLE devices, and sub-GHz signals — with GPS-tagged logging, in a case about **10 x 15 cm**.

## What the radios can and can't do

**ESP32-C3 / C5 / C6 class chips**
- **WiFi:** promiscuous mode captures management frames and data-frame headers. That gives access points (SSID, BSSID, channel, encryption, vendor, hidden SSIDs, RSSI), client devices (MAC, probed SSIDs, random-MAC detection) and which client talks to which AP. It can't decrypt traffic, and doesn't need to.
- **Bands:** the C3 and C6 are 2.4 GHz only. The **C5 adds 5 GHz**.
- **One radio per chip:** it can only listen on one channel at a time and shares that radio between WiFi and BLE. Splitting WiFi and BLE across two chips avoids time-slicing.
- **BLE:** advertisement scanning gives MAC, RSSI, name, manufacturer ID, and tracker detection (AirTag / Find My / Tile style). It can't do Classic Bluetooth.
- **Zigbee/Thread (802.15.4):** available on the C6, not implemented yet.

**Not possible with ESP32s:** true RF spectrum analysis (the "spectrum" node in the old build was really channel occupancy from WiFi frames), LoRa, ADS-B, cellular, GPS. Those need extra hardware: a CC1101 for sub-GHz, a GPS module, and an RTL-SDR (outside the PCB) for wideband/ADS-B.

**Legal note:** everything here is passive listening. Don't add deauth or injection features.

## Decisions (2026-09-20)
1. **Fresh start.** The ESP-NOW build never worked reliably.
2. **Wired UART links** replace ESP-NOW. The field nodes are receive-only and never transmit, which removes the timing and near-field interference problems.
3. **Hub is an ESP32-S3** with a separate 2.8" ILI9341 screen, instead of the ESP32-1732S019.
4. **Nodes:** XIAO ESP32-C5 (WiFi 2.4 + 5 GHz) and XIAO ESP32-C6 (BLE now, Zigbee/Thread later).
5. **GPS** on both nodes, so every sighting carries its own location and time.
6. **CC1101** replaces the faulty RXB6 sub-GHz receiver (hub SPI; not implemented in firmware yet).
7. **Power:** a ready-made LiPo charge/boost module (Adafruit PowerBoost 1000C), not a custom power stage. Only filtering, a fuse and back-feed diodes are added.
8. **Logging:** WiGLE-format CSV on microSD, uploadable to wigle.net.

## Hardware architecture
```
                GPS TX  ---------------+---------------+
                                       |               |
                              +--------v------+ +------v--------+
                              | XIAO ESP32-C5 | | XIAO ESP32-C6 |
                              | WiFi 2.4+5GHz | | BLE (+802.15.4)|
                              +------+--------+ +------+--------+
                                     | UART           | UART   (921600 baud)
                              +------v----------------v--------+
                              |  ESP32-S3 hub                   |
                              |  SPI bus: 2.8" TFT, microSD,    |
                              |           CC1101 sub-GHz        |
                              |  5 buttons, battery-low input   |
                              +---------------------------------+
   LiPo -> PowerBoost 1000C -> fuse -> 5 V rail -> each module (via a Schottky diode)
```

## Board layout (100 x 150 mm, 2 layers)
- Hub and screen on the top face; C5, C6, CC1101, SD, PowerBoost and battery on the bottom face; solid ground plane between.
- GPS at the top edge with clear sky and no copper under its antenna.
- C5 and C6 at opposite short edges (~10 cm apart) — a precaution now that neither transmits.
- Keep antenna areas of the XIAO boards and the CC1101 free of copper and traces.
- Each module's USB-C reachable from the case edge for flashing.

## Firmware status
Written in `../v2/`: shared protocol, node core (GPS + queue), WiFi node, BLE node, hub. **Nothing has been compiled or flashed yet** — the dev machine had no toolchain and the Debian flashing server (192.168.3.17) was unreachable. Expect a round of compile fixes. See `../v2/README.md` for the bring-up order.

Not implemented: CC1101 reception, 802.15.4 sniffing, per-region 5 GHz channel setup, battery-voltage reading.

## Known risks / things to check before ordering PCBs
1. **Footprints and pin orders** marked **[VERIFY]** in `PCB-BOM-AND-NETLIST.md` were written from memory. Check each against the datasheet or the physical part.
2. **ESP32-C5 Arduino support** for 5 GHz promiscuous mode is new. Fallback: a second C6 (2.4 GHz only).
3. **5 V budget:** ~900 mA peak against a 1 A module. Measure on the bench; move to a 2 A module if needed.
4. **3.3 V budget:** the S3 devkit's onboard regulator (~800 mA) feeds display, GPS, CC1101 and SD. If it browns out, add a separate 3.3 V regulator for the peripherals.
5. **CC1101 band** (433 vs 868/915 MHz) depends on your region.

## Next steps
1. Verify footprints against datasheets and draw the KiCad schematic from the netlist.
2. Get the firmware compiling (fix SSH to the Debian server, or install `arduino-cli` locally).
3. Bench bring-up on dev boards *before* fabricating the PCB: hub alone, then each node, then GPS, then a short drive test and a WiGLE upload.
4. Only after that, order the PCB.
