# CyberDeck v2 — exact BOM and netlist (KiCad input)

Authoritative parts list and pin-by-pin connections for the 100 x 150 mm board. Where this and `PCB-DESIGN.md` differ, **this file wins**. It matches the firmware in `../v2/`.

Items marked **[VERIFY]** are footprint/pinout facts recalled from memory of the manufacturer's documentation. Check each against the datasheet or the physical board before sending the PCB to fabrication.

## 1. Components

### Modules (all plug into socket headers — nothing is soldered down)

| Ref | Part | Qty | Board-side footprint | Notes |
|---|---|---|---|---|
| U1 | ESP32-S3-DevKitC-1, **N8R2** (or N16R2) | 1 | 2x 1x22 female 2.54 mm, rows ~22.86 mm apart **[VERIFY]** | Hub. Not R8/R16 — octal PSRAM eats GPIO33-37 |
| U2 | Seeed XIAO ESP32-C5 | 1 | 2x 1x7 female 2.54 mm, rows 15.24 mm apart **[VERIFY]** | WiFi 2.4 + 5 GHz node |
| U3 | Seeed XIAO ESP32-C6 | 1 | 2x 1x7 female 2.54 mm, rows 15.24 mm apart **[VERIFY]** | BLE node |
| DISP1 | 2.8" SPI TFT, ILI9341, 320x240, 14-pin header (MSP2807-type) | 1 | 1x14 male/female 2.54 mm | Touch pins left unconnected |
| U4 | CC1101 sub-GHz module (green 8-pin board, or E07-M1101D). **Band by region: 433 or 868/915 MHz** | 1 | 1x8 2.54 mm **[VERIFY pin order — it varies by vendor]** | Runs on 3.3 V only. Use SMA or spring antenna for the same band |
| U5 | u-blox NEO-M8N (or M10) GPS breakout with 4-pin header + ceramic patch antenna | 1 | 1x4 2.54 mm | Header: VCC, GND, TX, RX. Antenna on its own cable |
| J_SD | microSD breakout, 3.3 V logic (NO 5 V level shifter), 6-pin | 1 | 1x6 2.54 mm | GND, 3V3, MISO, MOSI, SCK, CS |
| PB1 | Adafruit PowerBoost 1000C (LiPo charger + 5 V boost, load-sharing) | 1 | 1x6 2.54 mm header pads **[VERIFY pad order]** | The only power stage. Rated ~1 A; see the power note |
| BT1 | 1S LiPo, 3.7 V, 2000-3000 mAh, with protection circuit, JST-PH 2 mm plug | 1 | none — plugs into PB1's own JST | |

### Board-level parts

| Ref | Part | Qty | Footprint | Notes |
|---|---|---|---|---|
| SW1-SW5 | 6x6 mm tactile switch, through-hole | 5 | 6x6 THT | UP, DOWN, LEFT, RIGHT, SELECT |
| SW_PWR | SPST slide switch | 1 | 2-pin THT | Shorts PB1 EN to GND = OFF |
| F1 | PTC resettable fuse, 1.1 A hold | 1 | radial 5 mm THT | On the 5 V rail |
| D1-D3 | Schottky diode 1N5819 or SS14 | 3 | DO-41 or SMA | One per module 5 V input (blocks USB back-feed) |
| C1 | 470 uF, 10 V+, electrolytic | 1 | radial 8 mm THT | 5 V bulk, at PB1 output |
| C2-C4 | 100 uF, 10 V+, electrolytic or tantalum | 3 | radial 5 mm THT | One at each module's 5 V input, after the diode |
| C5-C11 | 100 nF ceramic, X7R | 7 | 0805 | One at each module's 3V3/VCC (U1, U2, U3, DISP1, U4, U5, J_SD) |
| C12 | 10 uF ceramic | 1 | 0805 | GPS VCC |
| R1, R2 | 100 kOhm, 1% | 2 | 0805 | VBAT divider (2:1) |
| C13 | 100 nF | 1 | 0805 | VBAT divider filter (ADC pin to GND) |
| R3-R7 | 10 kOhm | 5 | 0805 | Optional: external pull-ups on buttons; firmware uses the internal ones, so **DNP** by default |
| R8 | 1 kOhm | 1 | 0805 | Power LED resistor |
| LED1 | LED, green | 1 | 3 mm THT | 5 V rail present |
| JP1 | 3-pad solder jumper | 1 | | Backlight: pad A = always on (3V3), pad B = GPIO7 control. **Default: bridge A** |
| J_SPARE | 1x6 pin header | 1 | 2.54 mm | 3V3, GND, GPIO18, GPIO21, GPIO48, 5V |
| TP1-TP3 | Test points | 3 | 1 mm pad | 5V, 3V3, GND |
| H1-H4 | M3 mounting holes | 4 | 3.2 mm | 4 mm in from each corner |

### Connectors / headers to buy
2.54 mm female header strips: 1x22 x2 (U1), 1x7 x4 (U2, U3), 1x14 x1 (DISP1), 1x8 x1 (U4), 1x4 x1 (U5), 1x6 x2 (J_SD, PB1). Plus male 1x6 for J_SPARE and pins for the modules that arrive without headers.

### Non-PCB items
- GPS patch antenna cable/mount (on the module), CC1101 antenna, optional U.FL antennas for the XIAO boards
- Enclosure, standoffs, M3 hardware
- USB-C cables (one per module for flashing)

## 2. Power tree

```
BT1 --(JST-PH)--> PB1 (PowerBoost 1000C)
PB1 5V --> F1 (1.1 A PTC) --> C1 470uF --> 5V_RAIL
5V_RAIL --> D1 --> C2 --> U2.5V   (XIAO C5)
5V_RAIL --> D2 --> C3 --> U3.5V   (XIAO C6)
5V_RAIL --> D3 --> C4 --> U1.5V   (S3 devkit)
5V_RAIL --> R8 --> LED1 --> GND
U1.3V3 --> 3V3_HUB  (powers DISP1, U4, U5, J_SD, J_SPARE)
GND common everywhere (one solid ground plane)
PB1.EN --- SW_PWR --- GND        (closed = off)
PB1.LBO --> U1.GPIO47            (active low: low battery)
PB1.BAT --> R1 --> VBAT_SENSE --> R2 --> GND ; C13 across R2 ; VBAT_SENSE --> U1.GPIO6
```

3.3 V load on U1's onboard regulator (~800 mA LDO): display ~100 mA + GPS ~50 mA + CC1101 ~35 mA + SD burst ~100 mA + S3 itself ~150-350 mA. Peak is close to the LDO limit. If the S3 browns out under load, power the display/SD from a separate 3.3 V regulator — don't reduce anything else first.

5 V budget: WiFi node ~250-300 mA peak, BLE node ~150 mA, S3 ~300-350 mA including 3V3 loads, display ~100 mA. **~900 mA-1 A peak against a 1 A module.** Measure it on the bench; if it is over 900 mA, use a 2 A boost module instead of PB1 (same nets, different footprint).

## 3. Netlist

### 3.1 Hub U1 (ESP32-S3-DevKitC-1) — GPIOs assigned

Pins avoided on purpose: GPIO0, 3, 45, 46 (strapping), 19/20 (USB), 26-32 (flash/PSRAM), 33-37 (kept free of octal PSRAM).

| Net | U1 GPIO | Connects to |
|---|---|---|
| SPI_SCLK | 12 | DISP1.SCK, J_SD.SCK, U4.SCK |
| SPI_MOSI | 11 | DISP1.SDI(MOSI), J_SD.MOSI, U4.MOSI(SI) |
| SPI_MISO | 13 | J_SD.MISO, U4.MISO(SO) — **DISP1.SDO is NOT connected** (avoids contention) |
| TFT_CS | 10 | DISP1.CS |
| TFT_DC | 9 | DISP1.DC/RS |
| TFT_RST | 8 | DISP1.RESET |
| TFT_BL | 7 | JP1 pad B (backlight control) |
| SD_CS | 14 | J_SD.CS |
| CC_CS | 15 | U4.CSN |
| CC_GDO0 | 16 | U4.GDO0 |
| CC_GDO2 | 17 | U4.GDO2 |
| WIFI_LINK_RX | 4 | U2.D6 (C5 TX) |
| WIFI_LINK_TX | 5 | U2.D7 (C5 RX) |
| BLE_LINK_RX | 1 | U3.D6 (C6 TX) |
| BLE_LINK_TX | 2 | U3.D7 (C6 RX) |
| BTN_UP | 38 | SW1 -> GND |
| BTN_DOWN | 39 | SW2 -> GND |
| BTN_LEFT | 40 | SW3 -> GND |
| BTN_RIGHT | 41 | SW4 -> GND |
| BTN_SEL | 42 | SW5 -> GND |
| BAT_LOW | 47 | PB1.LBO |
| VBAT_SENSE | 6 | midpoint of R1/R2 (firmware does not read it yet) |
| SPARE | 18, 21, 48 | J_SPARE |
| 5V | 5V pin | D3 cathode (via C4) |
| 3V3 | 3V3 pin (either) | 3V3_HUB net |
| GND | GND pins | GND |

Header positions on the DevKitC-1 (left row J1: 3V3, 3V3, RST, 4, 5, 6, 7, 15, 16, 17, 18, 8, 3, 46, 9, 10, 11, 12, 13, 14, 5V, GND; right row J3: GND, TX, RX, 1, 2, 42, 41, 40, 39, 38, 37, 36, 35, 0, 45, 48, 47, 21, 20, 19, GND, GND) **[VERIFY against the Espressif DevKitC-1 schematic before drawing the footprint]**.

### 3.2 Nodes U2 (XIAO C5) and U3 (XIAO C6) — identical wiring

XIAO header: left column top-to-bottom D0, D1, D2, D3, D4, D5, D6; right column top-to-bottom 5V, GND, 3V3, D10, D9, D8, D7 **[VERIFY]**.

| Net | Node pin | Connects to |
|---|---|---|
| LINK_TX | D6 | Hub RX (U2 -> U1.GPIO4, U3 -> U1.GPIO1) |
| LINK_RX | D7 | Hub TX (U2 <- U1.GPIO5, U3 <- U1.GPIO2) |
| GPS_RX | D2 | U5.TX (the same GPS TX line goes to both nodes) |
| 5V | 5V | D1 (U2) / D2 (U3) cathode, via C2/C3 |
| GND | GND | GND |
| (unused) | D0, D1, D3, D4, D5, D8, D9, D10, 3V3 | leave unconnected (route to test pads if you want them) |

The XIAO boards are selected in the firmware as `XIAO_ESP32C5` / `XIAO_ESP32C6` so the `D`-labels resolve correctly.

### 3.3 GPS U5

| U5 pin | Connects to |
|---|---|
| VCC | 3V3_HUB (through C12 10 uF + C10 100 nF to GND) — check your module accepts 3.3 V |
| GND | GND |
| TX | U2.D2 **and** U3.D2 |
| RX | not connected (leave a test pad; module is configured over USB-TTL beforehand) |

### 3.4 Display DISP1 (MSP2807-type, 14 pins) **[VERIFY pin order on your module]**

| Pin | Connects to |
|---|---|
| VCC | 3V3_HUB |
| GND | GND |
| CS | TFT_CS (GPIO10) |
| RESET | TFT_RST (GPIO8) |
| DC/RS | TFT_DC (GPIO9) |
| SDI (MOSI) | SPI_MOSI (GPIO11) |
| SCK | SPI_SCLK (GPIO12) |
| LED (backlight) | JP1 common pad (A -> 3V3_HUB, B -> TFT_BL GPIO7) |
| SDO (MISO) | **not connected** |
| T_CLK, T_CS, T_DIN, T_DO, T_IRQ | not connected |

### 3.5 CC1101 U4 **[VERIFY pin order]**

| Signal | Connects to |
|---|---|
| VCC | 3V3_HUB only (max 3.6 V — never 5 V) |
| GND | GND |
| SCK | SPI_SCLK |
| MOSI (SI) | SPI_MOSI |
| MISO (SO) | SPI_MISO |
| CSN | CC_CS (GPIO15) |
| GDO0 | CC_GDO0 (GPIO16) |
| GDO2 | CC_GDO2 (GPIO17) |

### 3.6 microSD J_SD

| Signal | Connects to |
|---|---|
| 3V3 | 3V3_HUB |
| GND | GND |
| CS | SD_CS (GPIO14) |
| MOSI | SPI_MOSI |
| MISO | SPI_MISO |
| SCK | SPI_SCLK |

### 3.7 Power stage PB1 (PowerBoost 1000C) **[VERIFY pad order]**

| PB1 pad | Connects to |
|---|---|
| 5V | F1 -> 5V_RAIL |
| GND | GND |
| BAT | R1 (VBAT sense) |
| EN | SW_PWR (other side GND) |
| LBO | U1.GPIO47 |
| VS | not connected |

### 3.8 Passives

| Ref | Between |
|---|---|
| F1 | PB1.5V and 5V_RAIL |
| C1 | 5V_RAIL to GND |
| D1, D2, D3 | anode 5V_RAIL, cathode to U2.5V / U3.5V / U1.5V |
| C2, C3, C4 | each module's 5V pin (after its diode) to GND |
| C5-C11 | 3V3/VCC of U1, U2, U3, DISP1, U4, U5, J_SD to GND, placed right at each pin |
| R1, R2 | PB1.BAT -> R1 -> VBAT_SENSE -> R2 -> GND |
| C13 | VBAT_SENSE to GND |
| R8, LED1 | 5V_RAIL -> R8 -> LED1 -> GND |
| SW1-SW5 | GPIO (see 3.1) to GND |
| J_SPARE | pin1 3V3_HUB, pin2 GND, pin3 GPIO18, pin4 GPIO21, pin5 GPIO48, pin6 5V_RAIL |

## 4. Layout rules (from the RF and power findings)

1. **Two layers, one solid ground plane** on the bottom layer. Do not split it.
2. **Keep-outs:** no copper, traces or components under the antenna end of U2/U3 (over the XIAO's own PCB antenna) or under the GPS patch antenna and its module.
3. **RF separation:** U2 and U3 at opposite short edges, ~10 cm apart. Both are receive-only, so this is a precaution.
4. **Placement:** hub U1 + DISP1 on the top face; U2, U3, U4, PB1 and the battery on the bottom face. GPS at the top edge with sky view. CC1101 antenna at a board edge, away from U2/U3 antennas.
5. **Power traces:** 1 mm minimum on 5V_RAIL, 1.5 mm from PB1 to C1. Put every bulk cap (C1-C4) close to the pin it feeds.
6. **USB-C access:** each module's USB-C must be reachable from the enclosure edge for flashing.
7. **SPI:** keep SCLK/MOSI short and away from the GPS. If the display SPI runs above ~40 MHz, add 33 Ohm series resistors on SCLK/MOSI (footprints only, DNP by default).

## 5. Open items before ordering boards
1. Footprint dimensions and pad order for every **[VERIFY]** item — do this from the datasheet or by measuring the physical part.
2. Measure real peak 5 V draw on the bench; switch to a 2 A boost module if > 900 mA.
3. Confirm the C5 can run 5 GHz promiscuous mode in Arduino before committing to it as the WiFi node.
4. Decide the CC1101 band (433 vs 868/915 MHz) from your region.
5. Decide whether the display backlight needs dimming; if so, replace JP1 with an AO3401 P-channel switch (not included here to keep the first spin simple).
