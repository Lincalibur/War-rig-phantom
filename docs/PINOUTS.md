# CyberDeck — pinouts, wiring, and power

Reference for physically building the deck: what boards you need, what gets wired to what, and how power is distributed. Pulled directly from the pin definitions in each sketch (`01-wifi-sniffer/`, `02-wifi-spectrum/`, `04-hub-console/`) — if you change a `_PIN` constant in firmware, update it here too.

**Sub-ghz node (Component 3) is excluded from the active build** — that board's hardware is confirmed faulty and isn't in use (see `03-subghz-control/README.md`). Its pinout is included at the bottom for reference in case replacement hardware arrives later.

## Boards you need

| # | Role | Board | Extra parts |
|---|---|---|---|
| 1 | WiFi sniffer | ESP32-C3 Dev Module (any generic C3 dev board with USB) | None — just the bare board |
| 2 | WiFi spectrum | ESP32-C3 Dev Module | 0.96" I2C SSD1306 OLED (128x64, 4-pin) |
| 4 | Hub console | Sunton/NDE3D **ESP32-1732S019** (integrated 1.9" ST7789 screen) | None — screen is factory-soldered onto this board |

Plus a shared power source (see below). No breadboard, no jumper wires beyond the 4 needed for the spectrum node's OLED — everything else is USB/battery power only.

## Power supply

All three boards run on 5V in via USB/VIN and self-regulate to 3.3V internally — no separate 3.3V rail to build.

**Current draw (5V rail):**

| Board | Typical | Peak |
|---|---|---|
| WiFi sniffer (ESP32-C3, promiscuous mode) | ~120mA | ~250-300mA |
| WiFi spectrum (ESP32-C3 + OLED) | ~100mA | ~180mA |
| Hub console (ESP32 + TFT) | ~200mA | ~400-500mA (backlight-dominated) |
| **Total** | **~420mA** | **~1A worst case** |

### Option A — powerbank (bring-up/testing, what you're using now)

Any 3-port (or more) USB powerbank, 5V/2A+ per port, 3A+ total. One USB cable per board. No wiring beyond the cables themselves.

**Known gotcha:** two of these boards (sniffer + spectrum) are both active 2.4GHz radios. Sitting them a few cm apart from each other — not from the hub — has been confirmed in the field to cause total ESP-NOW reception failure (near-field RF desense between the two boards), even though either one alone reaches the hub fine. **Keep the sniffer and spectrum boards at least 30-50cm apart from each other** regardless of power source. This matters for the PCB layout too — see below.

### Option B — single shared battery rail (portable v2)

```
LiPo battery (3.7V, single-cell, 2000-3000mAh)
        │
        ▼
Charge/boost module (IP5306-class "power bank IC" module —
NOT a bare TP4056/TP5100, those only charge; you need a module
that also boosts 3.7V → 5V for output while idle/not charging)
        │
        ▼  (5V out)
Distribution point (terminal block or small proto board)
        │
   ┌────┼────┬─────────┐
   ▼    ▼    ▼         │
 Sniffer Spectrum  Hub  (each board's 5V/VIN pin + a shared GND)
```

- Battery: single-cell 3.7V LiPo, 2000-3000mAh gives ~3-4hr runtime at the ~420mA typical draw above.
- Boost module: something like an IP5306-based board (2A+ output rating) — these combine charging AND boost-when-discharging in one module, which a bare TP4056/TP5100 does not do.
- Distribution: a small terminal block or perfboard fanning the boost module's 5V+GND out to each board's VIN/5V and GND pins. This is the piece a custom PCB replaces (see PCB section).

## Per-board pinout

### 1 — WiFi sniffer (ESP32-C3, `01-wifi-sniffer/firmware/wifi_sniffer/wifi_sniffer.ino`)

| Function | Pin | Notes |
|---|---|---|
| Status LED | GPIO8 | Onboard LED on most C3 dev boards — no external wiring. Toggles on each channel hop. Confirm your specific board's onboard LED pin if it doesn't blink. |
| Power | USB or VIN/GND | 5V in, self-regulated |

No other external connections. This board only needs power.

### 2 — WiFi spectrum (ESP32-C3, `02-wifi-spectrum/firmware/wifi_spectrum/wifi_spectrum.ino`)

| Function | ESP32-C3 pin | OLED pin | Notes |
|---|---|---|---|
| I2C data | GPIO8 | SDA | |
| I2C clock | GPIO9 | SCL | |
| 3.3V | 3V3 | VCC | OLED runs off 3.3V, not 5V |
| Ground | GND | GND | |
| Power | USB or VIN/GND | — | 5V in to the ESP32-C3 itself |

OLED is a standard 0.96" SSD1306, 128x64, I2C address `0x3C` (change `OLED_I2C_ADDR` in firmware if your unit uses `0x3D` instead). 4 wires total between the C3 and the OLED — this is the only board-to-peripheral wiring in the active build.

### 4 — Hub console (ESP32-1732S019, `04-hub-console/firmware/hub_console/hub_console.ino`)

This is a fully integrated board — the ST7789 screen is factory-soldered on, not a separate module you wire up. The pins below are internal to the board (the firmware needs to know them to drive the screen); **you don't wire anything here**, they're listed for reference/debugging only.

| Function | Pin |
|---|---|
| SPI bus | HSPI (SPI2_HOST) |
| SCLK | GPIO14 |
| MOSI | GPIO13 |
| DC (data/command) | GPIO2 |
| CS | GPIO15 |
| Reset | tied to EN (no separate reset pin) |
| Backlight | GPIO21 |

| Function | Pin | Notes |
|---|---|---|
| Power | USB or VIN/GND | 5V in, self-regulated |

Full datasheet: `docs/ESP32-1732S019_Datasheet.pdf`.

### Deferred — Sub-ghz node (ESP32-C3, not currently built — `03-subghz-control/firmware/subghz_node/subghz_node.ino`)

Only relevant once replacement hardware is sourced (current board is confirmed hardware-faulty, see `03-subghz-control/README.md`).

| Function | ESP32-C3 pin | RXB6 pin | Notes |
|---|---|---|---|
| Data (rc-switch decode) | GPIO4 | DATA (fanned to both pins below) | |
| Data (raw pulse capture) | GPIO5 | DATA | Same physical wire as above, just a second tap so both decode paths see every edge |
| Power | 5V | VCC | RXB6 is a 5V module |
| Ground | GND | GND | |
| Antenna | — | ANT pad | ~17.3cm wire, quarter-wave for 433MHz |

## PCB design considerations

You don't need a from-scratch ESP32 PCB — every board above is already a complete module with USB, regulator, and antenna built in. What you actually need is a **carrier/backplane board**: something that holds the three dev boards, distributes power to them, and (for the spectrum node) breaks out 4 wires to an OLED socket. Think of it as replacing the "powerbank + loose USB cables + jumper wires" setup with one board.

**What the carrier board needs to do:**
1. **Battery input + power distribution** — a JST-PH connector for the LiPo, the boost module (either place it directly on the PCB if you pick a module small enough to lay out, or just mount the ready-made IP5306 module and route its 5V/GND output pads to headers), and a 5V/GND rail fanned out to three sets of pins.
2. **Mounting for each dev board** — either female header sockets sized to each board's pin spacing (lets you unplug/swap a board without desoldering — recommended, since you're still actively debugging firmware) or direct through-hole pads if you're confident in the final pinout.
3. **OLED breakout for the spectrum node** — a 4-pin header (VCC/GND/SDA/SCL) positioned near that board's socket, wired to its GPIO8/9.
4. **Physical separation between the two 2.4GHz radios** — given the near-field interference you just hit, lay the sniffer and spectrum node's footprints out as far apart as the board size allows, ideally on opposite ends or opposite sides. If space is tight, a small grounded copper/foil shield between them is a cheap fallback, but distance is the simpler fix. Keep the hub anywhere — it doesn't transmit ESP-NOW, so it's not part of this problem.
5. **Basic battery protection** — a reverse-polarity diode or protected JST connector, and ideally the boost module you pick already includes low-voltage cutoff so the LiPo doesn't over-discharge.

**Practical suggestions:**
- **KiCad** (free, open source) is the standard tool for this scale of project — a 2-layer board is plenty, nothing here is high-speed or RF-critical enough to need more layers (the antennas are already on the dev boards, you're not routing RF traces yourself).
- Don't route copper pour directly under either C3 board's antenna area if you can avoid it — check that specific board's datasheet for its antenna keep-out zone.
- Screw terminals or a simple 2-pin JST for the battery/boost input, standard 0.1" (2.54mm) headers for everything else, keeps it hand-solderable with no exotic parts.
- Since you're still iterating on firmware, socketed headers (not soldered-down boards) are worth the extra height/cost — you'll want to pull a board to reflash it via the Debian server without unsoldering.

**Suggested next step, if you want it:** once you've picked exact dev board models (footprint/pin spacing varies slightly between C3 "dev module" variants) and a specific boost module, a bill-of-materials + a simple netlist (which pin connects to which) is straightforward to write up here — that's the input a KiCad schematic actually needs, and doesn't require me to run PCB CAD software to produce.
