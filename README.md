<div align="center">

# War-rig-phantom
### *A Portable OPSEC/OSINT Recon Deck*

**Passive WiFi · BLE · Sub-GHz Listening — Aggregated, Visualized, Yours**

![Status](https://img.shields.io/badge/status-in--development-yellow)
![Platform](https://img.shields.io/badge/platform-ESP32%20%2F%20ESP32--C3-blue)
![License](https://img.shields.io/badge/license-see--LICENSE-lightgrey)
![Passive Only](https://img.shields.io/badge/mode-passive%20listening%20only-success)

</div>

---

## Overview

**War-rig-phantom** is a modular cyberdeck built for passive RF reconnaissance across three domains — WiFi, Bluetooth Low Energy, and Sub-GHz — sensed by a trio of lightweight ESP32-C3 nodes and consolidated on a central ESP32 hub console with a live display.

Think of it as a distributed set of ears, each tuned to a different slice of spectrum, all reporting back to a single brain that turns raw signal noise into something you can actually *see* and *use*.

> **Scope & Ethics**
> Everything in this project is **passive listening / logging on your own gear.**
> - No deauth transmission
> - No packet injection
> - No touching networks you don't own
>
> This is a recon and awareness tool — not an attack platform.

---

## Architecture

Three sensor nodes, one job each — no display, no distractions, just clean RF capture broadcast over **ESP-NOW** to a hub that owns all the brains.

```
 ESP32-C3 #1          ESP32-C3 #2          ESP32-C3 #3
 WiFi Sniffer         BLE Hunter            Sub-GHz Node
 (headless)        (0.96" I2C OLED)         (headless)
       \                  |                    /
        \                 |  ESP-NOW           /
         \      broadcast deck_report_t       /
          \                |                 /
           \               |                /
            v              v               v
        +---------------------------------------+
        |            ESP32 Hub Console           |
        |   display · aggregation · dashboard    |
        |              · alerts                  |
        +---------------------------------------+
```

**Design philosophy:** sensor nodes stay dumb and cheap; the hub stays smart. Each node does one thing — listen — and pushes structured reports outward. All the interesting logic (aggregation, alerting, UI) lives in exactly one place.

> **Evolution note:** This is an ESP-NOW-consolidated redesign — see the `esp-now-consolidation` branch and [`COMPONENTS.md`](COMPONENTS.md). An earlier revision split sub-GHz sensing and menu/control onto an Arduino Uno + big LCD. That path's been retired: sub-GHz now runs on a third ESP32-C3 node like its siblings, and the hub owns all display/control duties. The legacy Uno sketch still lives in git history / on `main` for reference.

---

## Components

| # | Component | Hardware | Status |
|---|---|---|---|
| 1 | [WiFi Sniffer Node](01-wifi-sniffer/README.md) | ESP32-C3 #1 | In progress — compiles clean, not yet flashed/tested |
| 2 | [BLE Hunter Node](02-ble-hunter/README.md) | ESP32-C3 #2 + 0.96" I2C OLED | In progress — core scan/log confirmed live, persistence + tracker-flag logic flashed, live re-confirmation pending |
| 3 | [Sub-GHz Node](03-subghz-control/README.md) | ESP32-C3 #3 (headless) | In progress — compiles clean, not yet flashed/tested |
| 4 | [Hub Console](04-hub-console/README.md) | ESP32 + display | In progress — compiles clean, not yet flashed/tested |
| 5 | [Integration Layer](05-integration/README.md) | All of the above | Not started — blocked on 1–4 passing standalone |

**Legend:** Not started · In progress · Done

Full build order and dependency chain live in [`COMPONENTS.md`](COMPONENTS.md). Every numbered folder is a standalone, individually-testable component with its own build checklist — nothing here has to be built in one heroic sitting.

---

## Gallery

> *Screenshots and build photos coming soon — deck is still being assembled.*

<div align="center">
<sub>placeholder — hub console dashboard</sub>
<br>
<sub>placeholder — node enclosures / wiring</sub>
<br>
<sub>placeholder — full deck in the field</sub>
</div>

---

## Docs

| Doc | What's in it |
|---|---|
| [`docs/opsec-osint-deck-plan.md`](docs/opsec-osint-deck-plan.md) | Original implementation plan |
| [`docs/deck-component-availability.md`](docs/deck-component-availability.md) | Parts sourcing |
| [`docs/debian-esp32-server-guide.md`](docs/debian-esp32-server-guide.md) | Remote flashing station setup |

---

## License

See [LICENSE](LICENSE).

<div align="center">
<sub>Built one node at a time.</sub>
</div>
