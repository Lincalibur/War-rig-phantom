# SPECTRE carrier — connector pinout (for wiring / routing)

Every net is already in `spectre.kicad_pcb` (ratsnest shown on F8). This table is the human-readable
map so you know what each pad is. ESP32 positions use the **canonical DevKitC-V4 order** — verify
against your physical board. `—` = no connection (leave unrouted).

## J1 — ESP32-WROOM-32U, LEFT header (1×19)  *(on the BACK)*
| Pin | GPIO / func | Net | Goes to |
|----:|---|---|---|
| 1 | 3V3 (reg out) | `+3V3` | TFT logic (J3.4), expansion, decoupling |
| 2 | EN | — | |
| 3 | IO36 (in-only) | `EXP_IN3` | J8.10 |
| 4 | IO39 (in-only) | `EXP_IN4` | J8.11 |
| 5 | IO34 (in-only) | `EXP_IN1` | J8.8 |
| 6 | IO35 (in-only) | `EXP_IN2` | J8.9 |
| 7 | IO32 | `TFT_RST` | J3 shield A4 (RST) |
| 8 | IO33 | `TFT_CS` | J3 shield A3 (CS) |
| 9 | IO25 | `TFT_D3` | J3 shield D3 |
| 10 | IO26 | `TFT_D2` | J3 shield D2 |
| 11 | IO27 | `TFT_D6` | J3 shield D6 |
| 12 | IO14 | `TFT_D7` | J3 shield D7 |
| 13 | IO12 ⚠strap | `TFT_D0` | J3 shield D8 (=LCD_D0); R1 pulldown (DNP) |
| 14 | GND | `GND` | |
| 15 | IO13 | `TFT_D1` | J3 shield D9 (=LCD_D1) |
| 16–18 | IO9/10/11 | — | internal flash — **do not use** |
| 19 | 5V / VIN | `+5V` | power in |

## J2 — ESP32-WROOM-32U, RIGHT header (1×19)  *(on the BACK)*
| Pin | GPIO / func | Net | Goes to |
|----:|---|---|---|
| 1 | GND | `GND` | |
| 2 | IO23 | `EXP_MOSI` | J8.5 |
| 3 | IO22 (TX2) | `GPS_TX` | J7.4 (GPS RXD) |
| 4 | IO1 / TX0 | — | USB serial — leave free |
| 5 | IO3 / RX0 | — | USB serial — leave free |
| 6 | IO21 (RX2) | `GPS_RX` | J7.3 (GPS TXD) |
| 7 | IO19 | `EXP_MISO` | J8.6 |
| 8 | IO18 | `EXP_SCK` | J8.4 |
| 9 | IO5 | `EXP_CS0` | J8.7 |
| 10 | IO17 | `TFT_D4` | J3 shield D4 |
| 11 | IO16 | `TFT_D5` | J3 shield D5 |
| 12 | IO4 | `TFT_WR` | J3 shield A1 (WR) |
| 13 | IO0 ⚠strap | — | boot — leave free |
| 14 | IO2 ⚠strap | `TFT_RD` | J3 shield A0 (RD) |
| 15 | IO15 | `TFT_DC` | J3 shield A2 (DC) |
| 16–18 | IO8/7/6 | — | internal flash — **do not use** |
| 19 | GND | `GND` | |

## J3 — TFT (ILI9488 parallel), one **Arduino-UNO-R3** shield footprint — front
The physical shield plugs straight in. Only the pins below are wired (mcufriend mapping); all other
shield pins (D0/D1, D10–D13 SD, A5, AREF, SDA/SCL, IOREF, RESET, VIN, NC) are left unconnected.
| Shield pin | Pad # | Net |
|---|---:|---|
| **3V3** | 4 | `+3V3` — TFT logic rail (**the white-screen fix — must be fed**) |
| **5V** | 5 | `+5V` |
| GND | 6, 7, 29 | `GND` |
| A0 (LCD_RD) | 9 | `TFT_RD` |
| A1 (LCD_WR) | 10 | `TFT_WR` |
| A2 (LCD_RS/DC) | 11 | `TFT_DC` |
| A3 (LCD_CS) | 12 | `TFT_CS` |
| A4 (LCD_RST) | 13 | `TFT_RST` |
| D2–D7 (LCD_D2–D7) | 17–22 | `TFT_D2`…`TFT_D7` |
| D8 (LCD_D0) | 23 | `TFT_D0` |
| D9 (LCD_D1) | 24 | `TFT_D1` |

## J7 — NEO-7M GPS (1×5), top
| Pin | Label | Net |
|----:|---|---|
| 1 | VCC | `+5V` *(move to +3V3 if no fix on 5 V)* |
| 2 | GND | `GND` |
| 3 | TXD | `GPS_RX` → ESP32 IO21 |
| 4 | RXD | `GPS_TX` ← ESP32 IO22 |
| 5 | PPS | — |

## J8 — Expansion (1×12), bottom
`+5V` · `+3V3` · `GND` · `EXP_SCK`(IO18) · `EXP_MOSI`(IO23) · `EXP_MISO`(IO19) · `EXP_CS0`(IO5) · `EXP_IN1`(IO34) · `EXP_IN2`(IO35) · `EXP_IN3`(IO36) · `EXP_IN4`(IO39) · `GND`
> VSPI bus + 4 input-only lines. Enough for **one** SPI radio at a time (CC1101 **or** one NRF24), not the full 3×NRF24+CC1101 shield — see README "GPIO budget".

## J9 — 5 V power in (1×2)
1 `+5V` · 2 `GND`  (from the 1S 18650 + boost module)

## Passives
| Ref | Net A | Net B | Note |
|---|---|---|---|
| R1 10k | `TFT_D0` (IO12) | `GND` | **DNP** — fit only if boot/flash flakes (strapping pin) |
| R2 1k | `+5V` | `LED_A` | power LED series |
| D1 LED | `LED_A` | `GND` | power indicator |
| C1 100nF | `+3V3` | `GND` | ESP32 |
| C2 100nF | `+5V` | `GND` | TFT 5V |
| C3 100nF | `+3V3` | `GND` | TFT logic |
| C4 100nF | `+5V` | `GND` | GPS |
| C5 10µF | `+5V` | `GND` | 5V bulk |
| C6 10µF | `+3V3` | `GND` | 3V3 bulk |
| C7 100nF | `+5V` | `GND` | expansion |
| C8 10µF | `+3V3` | `GND` | expansion radio bulk |
