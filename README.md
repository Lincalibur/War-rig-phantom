<div align="center">

# War-rig-phantom
### *A Portable OPSEC/OSINT Recon Deck*

**Passive WiFi, BLE and sub-GHz listening with GPS-tagged logging, in a 22 cm clamshell case**

![Status](https://img.shields.io/badge/status-budget%20v1%20build-yellow)
![Platform](https://img.shields.io/badge/platform-ESP32--S3%20%2F%20C3%20%2F%20ESP32-blue)
![License](https://img.shields.io/badge/license-see--LICENSE-lightgrey)
![Passive Only](https://img.shields.io/badge/mode-passive%20listening%20only-success)

**[Build page](https://lincalibur.github.io/War-rig-phantom/)**: boards, case layout, parts checklist and wiring

</div>

---

## Overview

**War-rig-phantom** is a portable counter-surveillance deck built into a 22 × 22 cm clamshell case. Two receive-only ESP32 nodes (WiFi and BLE) and a CC1101 sub-GHz radio feed an ESP32-S3 hub, which logs WiGLE-format CSV to microSD and flags devices that keep following you. Each scan module has its own screen in the lid. A GPS module feeds both nodes, so every sighting carries its own position and time. It runs from a 2S2P 18650 pack. This is the budget v1 build; a Raspberry Pi 5 takes over as hub later.

> **Scope & Ethics**
> Everything in this project is **passive listening and logging.**
> - No deauth transmission
> - No packet injection
> - No touching networks you don't own
>
> This is a recon and awareness tool, not an attack platform.

## Architecture

```
  GPS (NEO-7M) TX --+--> WIFI-NODE  ESP32-C3 ---------------------UART--+
                    +--> BLE-SCAN   ESP32 + built-in 1.9" screen --UART--+
                                                                         v
                         HUB  ESP32-S3: microSD, CC1101 433 MHz, LED + piezo alerts
                                                                         |  row stream
                                                                         v
                         READOUT  Arduino UNO + 3.5" TFT (hub display)

  STATUS  ESP32-C3 + OLED (standalone counters)
  2S2P 18650 -> BMS -> fuse -> master switch -> 5 V buck -> one switch per module
```

The nodes never transmit. They report to the hub over wired UART, which fixes the timing and near-field interference problems that sank the v1 ESP-NOW design. Board names (`HUB`, `READOUT`, `BLE-SCAN`, `WIFI-NODE`, `STATUS`) are defined in [`docs/hardware-layout.md`](docs/hardware-layout.md#board-names).

## Repository

| Path | What's in it |
|---|---|
| [`docs/OVERVIEW.md`](docs/OVERVIEW.md) | **Start here.** Goals, decisions, capabilities, status and next steps |
| [`docs/hardware-layout.md`](docs/hardware-layout.md) | The current build: board names, case layout, power, wiring and parts list |
| [`docs/BOARD-BLE-SCAN-ESP32-1732S019.md`](docs/BOARD-BLE-SCAN-ESP32-1732S019.md) | Spec for the `BLE-SCAN` board |
| [`docs/build-page.html`](docs/build-page.html) | Source of the build page (deployed by `.github/workflows/pages.yml`) |
| [`docs/PCB-BOM-AND-NETLIST.md`](docs/PCB-BOM-AND-NETLIST.md), [`docs/esp32-hub-board.html`](docs/esp32-hub-board.html), [`KiCad Design/`](KiCad%20Design/) | Shelved single-PCB design. The S3 hub pin assignments are still used |
| [`v2/`](v2/README.md) | Firmware and bench prototypes (hub and nodes written, not yet compiled) |
| [`docs/debian-esp32-server-guide.md`](docs/debian-esp32-server-guide.md) | Remote compile/flash station over SSH |

The v1 build (ESP-NOW C3 nodes and an ESP32-1732S019 hub) was removed from the tree. Recover it with `git checkout v1-espnow-archive`.

## Status

| Area | State |
|---|---|
| Case layout and parts list | ✅ planned (2026-10-09); nde3d cart about R1,390 |
| Bench prototypes | ✅ WiFi readout working (C3 scanner to UNO + 3.5" TFT) |
| Power system | ⬜ parts to order; bench test first |
| Firmware | 🟨 hub and nodes written for the old PCB; need retargeting to the boards above, then a first compile |
| Raspberry Pi 5 hub | ⬜ later phase |

## License

See [LICENSE](LICENSE).
