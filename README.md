<div align="center">

# War-rig-phantom
### *A Portable OPSEC/OSINT Recon Deck*

**Passive WiFi, BLE and sub-GHz listening with GPS-tagged logging, on one handheld PCB**

![Status](https://img.shields.io/badge/status-PCB%20design-yellow)
![Platform](https://img.shields.io/badge/platform-ESP32--S3%20%2F%20C3%20%2F%20H2-blue)
![License](https://img.shields.io/badge/license-see--LICENSE-lightgrey)
![Passive Only](https://img.shields.io/badge/mode-passive%20listening%20only-success)

**[Interactive build page](https://lincalibur.github.io/War-rig-phantom/)**: BOM checklist, board layout and searchable pin tables

</div>

---

## Overview

**War-rig-phantom** is a handheld wardriving deck. Two receive-only ESP32 nodes (WiFi and BLE) and a CC1101 sub-GHz radio feed an ESP32-S3 hub. The hub shows everything on a 2.8" display and logs WiGLE-format CSV to microSD. A GPS module feeds both nodes, so every sighting carries its own position and time. It all runs from one 18650 cell.

> **Scope & Ethics**
> Everything in this project is **passive listening and logging.**
> - No deauth transmission
> - No packet injection
> - No touching networks you don't own
>
> This is a recon and awareness tool, not an attack platform.

## Architecture

```
            GPS (NEO-7M) TX ------+-----------------+
                                  |                 |
                        +---------v------+  +-------v--------+
                        | ESP32-C3       |  | ESP32-H2       |
                        | WiFi node      |  | BLE node       |
                        +-------+--------+  +-------+--------+
                                |  wired UART       |
                        +-------v-------------------v--------+
                        |  ESP32-S3 hub                      |
                        |  2.8" TFT - microSD - CC1101 433M  |
                        |  5-way nav - battery sense         |
                        +------------------------------------+
     18650 -> IP5310 charge/boost board -> 5 V rail -> each module
```

The nodes never transmit. They report to the hub over wired UART, which fixes the timing and near-field interference problems that sank the v1 ESP-NOW design.

## Repository

| Path | What's in it |
|---|---|
| [`docs/OVERVIEW.md`](docs/OVERVIEW.md) | **Start here.** Goals, decisions, capabilities, board rules, status and next steps |
| [`docs/PCB-BOM-AND-NETLIST.md`](docs/PCB-BOM-AND-NETLIST.md) | Authoritative parts list (nde3d.co.za) and pin-by-pin netlist |
| [`docs/esp32-hub-board.html`](docs/esp32-hub-board.html) | Source of the interactive build page (deployed by `.github/workflows/pages.yml`) |
| [`KiCad Design/`](KiCad%20Design/) | KiCad project |
| [`v2/`](v2/README.md) | Hub and node firmware (written, not yet compiled; node pins need remapping) |
| [`docs/debian-esp32-server-guide.md`](docs/debian-esp32-server-guide.md) | Remote compile/flash station over SSH |

The v1 build (ESP-NOW C3 nodes and an ESP32-1732S019 hub) was removed from the tree. Recover it with `git checkout v1-espnow-archive`.

## Status

| Area | State |
|---|---|
| BOM and netlist | ✅ done (2026-09-25, nde3d parts); pinouts marked [VERIFY] need checking against real parts |
| KiCad schematic | 🟨 in progress |
| PCB layout | ⬜ not started (board size to confirm) |
| Firmware | 🟨 written for XIAO C5/C6; needs a remap to C3 PRO Mini / H2 SuperMini, then a first compile |

## License

See [LICENSE](LICENSE).
