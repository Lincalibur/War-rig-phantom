<div align="center">

# War-rig-phantom
### *A Portable OPSEC/OSINT Recon Deck*

**Passive WiFi Sniffing, Spectrum Analysis & Sub-GHz Listening — Aggregated, Visualized, Yours**

![Status](https://img.shields.io/badge/status-in--development-yellow)
![Platform](https://img.shields.io/badge/platform-ESP32%20%2F%20ESP32--C3-blue)
![License](https://img.shields.io/badge/license-see--LICENSE-lightgrey)
![Passive Only](https://img.shields.io/badge/mode-passive%20listening%20only-success)

</div>

---

## Overview

**War-rig-phantom** is a modular cyberdeck built for passive RF reconnaissance — WiFi sniffing, WiFi channel/spectrum analysis, and (planned) sub-GHz listening — sensed by lightweight ESP32-C3 field nodes and consolidated on a central ESP32 hub console with a live display.

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

Field nodes stay dumb and cheap; the hub stays smart. Each node does one thing — listen — and pushes structured reports outward over **ESP-NOW**. All the interesting logic (aggregation, alerting, UI) lives in exactly one place: the hub.

```
 ESP32-C3 #1          ESP32-C3 #2
 WiFi Sniffer         WiFi Spectrum
 (headless)           (headless on current board)
       \                  |
        \                 |  ESP-NOW
         \      broadcast deck_report_t /
          \                |           /
           \               |          /
            v              v
        +---------------------------------------+
        |            ESP32 Hub Console           |
        |   display · aggregation · dashboard    |
        +---------------------------------------+
```

**Design philosophy:** field nodes stay dumb and cheap; the hub stays smart. Each node does one thing — listen — and pushes structured reports outward. All the interesting logic (aggregation, alerting, UI) lives in exactly one place.

> **Evolution note:** This is an ESP-NOW-consolidated redesign — see the `esp-now-consolidation` branch and [`COMPONENTS.md`](COMPONENTS.md). An earlier revision split sub-GHz sensing and menu/control onto an Arduino Uno + big LCD; that path's been retired. The original BLE hunter node (Component 2) was later repurposed into a WiFi channel/spectrum analyzer. The sub-GHz node (Component 3) is currently excluded from the active build — its hardware is confirmed faulty and needs replacement — so the deck currently runs on 2 field nodes + the hub. Legacy sketches still live in git history / on `main` for reference.

---

## Components

| # | Component | Hardware | Status |
|---|---|---|---|
| 1 | [WiFi Sniffer Node](01-wifi-sniffer/README.md) | ESP32-C3 #1 | 🟩 flashed, confirmed detecting real networks/devices standalone |
| 2 | [WiFi Spectrum Node](02-wifi-spectrum/README.md) | ESP32-C3 #2 (no OLED on the current board) | 🟩 flashed, confirmed scanning + sending standalone |
| 3 | [Sub-GHz Node](03-subghz-control/README.md) | ESP32-C3 #3, no display | ⬜ excluded from the active build — hardware confirmed faulty, needs replacement |
| 4 | [Hub Console](04-hub-console/README.md) | ESP32 + screen | 🟩 flashed, dashboard confirmed rendering, receives either field node individually |
| 5 | [Integration Layer](05-integration/README.md) | 1, 2, 4 | 🟨 each node reaches the hub fine alone; **known issue: both field nodes active at once currently breaks reception** — suspected near-field RF interference between the two closely-spaced 2.4GHz radios, not yet confirmed fixed by physical separation |

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
| [`docs/PINOUTS.md`](docs/PINOUTS.md) | Pin layouts, power supply wiring (powerbank or battery+boost), PCB design considerations |
| [`docs/opsec-osint-deck-plan.md`](docs/opsec-osint-deck-plan.md) | Original implementation plan |
| [`docs/deck-component-availability.md`](docs/deck-component-availability.md) | Parts sourcing |
| [`docs/debian-esp32-server-guide.md`](docs/debian-esp32-server-guide.md) | Remote flashing station setup |

---

## License

See [LICENSE](LICENSE).

<div align="center">
<sub>Built one node at a time.</sub>
</div>
