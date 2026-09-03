# War-rig-phantom (CyberDeck)

A portable OPSEC/OSINT recon deck: passive WiFi (sniffing + channel/spectrum analysis) and sub-GHz listening across three ESP32-C3 sensor nodes, aggregated on an ESP32 hub console with a display.

**Scope:** everything here is passive listening / logging on your own gear. No deauth transmission, no packet injection, no touching networks you don't own.

## Architecture

Three ESP32-C3 nodes each scan one RF domain and report over ESP-NOW broadcast to a central ESP32 hub, which owns all display, menu, and aggregation logic.

```
 ESP32-C3 #1          ESP32-C3 #2          ESP32-C3 #3
 WiFi sniffer         WiFi spectrum         Sub-GHz node
 (no display)      (0.96" I2C OLED)        (no display)
       \                  |                    /
        \                 | ESP-NOW            /
         \      broadcast deck_report_t       /
          \                |                 /
                    ESP32 Hub Console
                    (screen, aggregation,
                     dashboard, alerts)
```

This is an ESP-NOW-consolidated redesign (see `esp-now-consolidation` branch / `COMPONENTS.md`): an earlier design put sub-GHz sensing and a menu/control surface on an Arduino Uno + big LCD. That's been dropped — sub-GHz sensing now runs on a third ESP32-C3 node like the other two, and the hub owns all display/control. The old Uno sketch remains in git history / on `main` if needed.

## Components

| # | Component | Device | Status |
|---|---|---|---|
| 1 | [WiFi Sniffer Node](01-wifi-sniffer/README.md) | ESP32-C3 #1 | 🟩 flashed, confirmed detecting real networks/devices standalone |
| 2 | [WiFi Spectrum Node](02-wifi-spectrum/README.md) | ESP32-C3 #2 (no OLED on the current board) | 🟩 flashed, confirmed scanning + sending standalone |
| 3 | [Sub-GHz Node](03-subghz-control/README.md) | ESP32-C3 #3, no display | ⬜ excluded from the active build — hardware confirmed faulty, needs replacement |
| 4 | [Hub Console](04-hub-console/README.md) | ESP32 + screen | 🟩 flashed, dashboard confirmed rendering, receives either field node individually |
| 5 | [Integration Layer](05-integration/README.md) | 1, 2, 4 | 🟨 each node reaches the hub fine alone; **known issue: both field nodes active at once currently breaks reception** — suspected near-field RF interference between the two closely-spaced 2.4GHz radios, not yet confirmed fixed by physical separation |

Full details and build order live in [COMPONENTS.md](COMPONENTS.md). Each numbered folder is a standalone, individually-testable component with its own build checklist.

## Docs

- [`docs/PINOUTS.md`](docs/PINOUTS.md) — pin layouts, power supply wiring (powerbank or battery+boost), and PCB design considerations
- [`docs/opsec-osint-deck-plan.md`](docs/opsec-osint-deck-plan.md) — original implementation plan
- [`docs/deck-component-availability.md`](docs/deck-component-availability.md) — parts sourcing
- [`docs/debian-esp32-server-guide.md`](docs/debian-esp32-server-guide.md) — remote flashing station setup

## License

See [LICENSE](LICENSE).
