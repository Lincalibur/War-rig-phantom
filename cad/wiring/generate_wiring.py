#!/usr/bin/env python3
"""Draw the HUB wiring diagrams for the ESP32-DIV v1 handheld build.

    pip install matplotlib
    python generate_wiring.py

The HUB is built as two assemblies joined by one 6-wire cable (J1):

    ESP32 + touchscreen    ESP32-WROOM-32U dev board, 2.4" ILI9341 touch+SD TFT,
                           power chain, antenna
    PROTOBOARD             UNO proto shield (not plugged into an UNO) carrying the
                           GY-NEO-7M GPS, the PCF8574 button expander and 5 buttons

Output (all in ../../docs/, PNG previews in preview/):
    wiring-overview.svg    the two assemblies and the J1 cable between them
    wiring.svg             ESP32 + touchscreen, pin by pin
    wiring-protoboard.svg  protoboard layout, pin by pin

Pins come from cifertech's ESP32-DIV firmware, v1 target (BOARD_ESP32_DIV_V1):
shared.h (touch, SD, backlight, PCF8574 buttons, GPS UART) and
Libraries/User_Setup v1.h (TFT bus). I2C uses the ESP32 defaults SDA 21 / SCL 22
(the firmware calls Wire.begin() without pins). Drawings are logical: wire by the
silkscreen labels, not by where a pin sits in the picture.
"""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Circle, FancyBboxPatch

HERE = Path(__file__).resolve().parent
DOCS = HERE.parent.parent / "docs"

FILL = {"screen": "#BFD7EA", "compute": "#CFCDE8", "rf": "#F2D7A6", "ui": "#E6D3EC",
        "power": "#F4C7B8", "note": "#F7F3E3", "pcb": "#DCE8F3", "cable": "#E4E4E4",
        "white": "#FFFFFF"}
RED, AMBER, GREEN, BLUE, VIOLET, GREY, BLACK, TEAL = (
    "#C8392B", "#B07A0E", "#1C8A6B", "#2B6CB0", "#6A4FC4", "#555555", "#222222", "#0E7C86")

# (part pin label, ESP32 GPIO, colour). Rows run top to bottom; wires are straight.
TFT_ROWS = [
    ("CS", 15, BLUE), ("RESET", 0, BLUE), ("DC", 2, BLUE), ("SDI (MOSI)", 13, BLUE),
    ("SCK", 14, BLUE), ("LED", 32, AMBER), ("SDO (MISO)", 12, BLUE),
    ("T_CS", 33, VIOLET), ("T_CLK + SD_SCK", 18, VIOLET), ("T_DIN + SD_MOSI", 23, VIOLET),
    ("T_DO + SD_MISO", 19, VIOLET), ("SD_CS", 5, VIOLET),
]

# The J1 cable between the two assemblies: (pin, signal, ESP32 end, protoboard end, colour)
J1 = [
    (1, "3V3", "3V3", "3V3 rail", RED),
    (2, "GND", "GND", "GND rail", BLACK),
    (3, "SDA", "GPIO21", "PCF8574 SDA", GREEN),
    (4, "SCL", "GPIO22", "PCF8574 SCL", GREEN),
    (5, "GPS TX", "GPIO3 (RX0)", "GPS TXD (via JP1)", TEAL),
    (6, "GPS RX", "GPIO1 (TX0)", "GPS RXD (via JP1)", TEAL),
]

# PCF8574 P pin -> button name (v1 firmware)
BUTTON_OF = {3: "DOWN", 4: "LEFT", 5: "RIGHT", 6: "UP", 7: "SELECT"}
BUTTON_COLOUR = {"UP": "#C2185B", "DOWN": "#7B1FA2", "LEFT": "#283593", "RIGHT": "#00838F",
                 "SELECT": "#E65100"}


def box(ax, x, y, w, h, key, title=None, body=None, size=10, lw=1.0, ls="-"):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=.8",
                                fc=FILL[key], ec="#333333", lw=lw, ls=ls))
    if title:
        ax.text(x + w / 2, y + h - 2.2, title, ha="center", va="center", fontsize=size, weight="bold")
    if body:
        ax.text(x + w / 2, y + h / 2 - (1.6 if title else 0), body, ha="center", va="center", fontsize=8.8)


def wire(ax, pts, colour, label=None, at=None, width=1.6, arrow=False, fs=8.3):
    p = np.array(pts, dtype=float)
    ax.plot(p[:, 0], p[:, 1], color=colour, lw=width, solid_capstyle="round", zorder=3)
    if arrow:
        ax.annotate("", xy=p[-1], xytext=p[-2],
                    arrowprops=dict(arrowstyle="-|>", color=colour, lw=width), zorder=4)
    if label:
        ax.text(*at, label, fontsize=fs, color=colour, ha="center", va="center",
                bbox=dict(fc="white", ec="none", pad=.5), zorder=5)


def dot(ax, x, y, colour):
    ax.add_patch(Circle((x, y), .55, fc=colour, ec=colour, zorder=6))


def pin(ax, x, y, label, side="right", colour=BLACK, fs=8.3):
    """A header pin: small square, label inside the part on the given side."""
    ax.add_patch(FancyBboxPatch((x - .55, y - .55), 1.1, 1.1, boxstyle="square,pad=0",
                                fc="#F0C040", ec="#333333", lw=.7, zorder=7))
    ha = "right" if side == "right" else "left"
    off = -1.2 if side == "right" else 1.2
    ax.text(x + off, y, label, ha=ha, va="center", fontsize=fs, family="monospace", color=colour)


def save(fig, name):
    fig.tight_layout()
    fig.savefig(DOCS / f"{name}.svg")
    (HERE / "preview").mkdir(exist_ok=True)
    fig.savefig(HERE / "preview" / f"{name}.png", dpi=90)
    plt.close(fig)


# ---------------------------------------------------------------------------
# 1. ESP32 + touchscreen
# ---------------------------------------------------------------------------
def draw_esp32_tft():
    fig, ax = plt.subplots(figsize=(13, 9.4))
    ax.set_xlim(0, 130); ax.set_ylim(0, 94); ax.axis("off")
    ax.text(2, 92.5, "ESP32 + touchscreen assembly", fontsize=12, weight="bold", va="top")

    # --- Power chain, top row ---------------------------------------------
    box(ax, 2, 80, 22, 8, "power", None, "3x 18650 2200 mAh\nin parallel (1S3P)")
    box(ax, 30, 80, 16, 8, "ui", None, "Master switch")
    box(ax, 52, 80, 26, 8, "power", None, "5V 2A charge/boost module\nBAT in, 5 V out")
    box(ax, 84, 80, 22, 8, "power", None, "USB-C breakout\n(charge input pads)")
    wire(ax, [(24, 84), (30, 84)], RED, arrow=True)
    wire(ax, [(46, 84), (52, 84)], RED, arrow=True)
    wire(ax, [(84, 84), (78, 84)], RED, arrow=True)
    ax.text(27, 86.6, "BAT+", fontsize=7.5, color=RED, ha="center")

    # 5 V rail: module out -> ESP32 VIN and TFT VCC.
    wire(ax, [(65, 80), (65, 76), (17, 76), (17, 73)], RED, "5 V rail", (40, 76))
    wire(ax, [(65, 76), (65, 72)], RED)
    ax.text(67.5, 74, "VIN", fontsize=8.3, color=RED)

    # --- ESP32 -----------------------------------------------------------
    ex, ey, ew, eh = 50, 9.5, 30, 62.5
    box(ax, ex, ey, ew, eh, "compute")
    ax.text(ex + ew / 2, ey + eh - 3, "HUB: ESP32-WROOM-32U", ha="center", fontsize=10.5, weight="bold")
    ax.text(ex + ew / 2, ey + eh - 6.2, "dev board, ESP32-DIV v1 firmware", ha="center", fontsize=8.5)
    ax.text(ex + ew / 2, ey + 2.6, "3.3 V logic, common GND", ha="center", va="center",
            fontsize=8.3, color=GREY)

    # --- TFT: 2.4" ILI9341 + XPT2046 touch + SD, left ----------------------
    tx, tw = 2, 30
    top = 62
    step = 4.0
    box(ax, tx, top - step * len(TFT_ROWS) - 1.5, tw, step * len(TFT_ROWS) + 12.5, "screen",
        '2.4" ILI9341 TFT\ntouch + SD', size=9.5)
    ax.text(tx + tw / 2, top + 4.0, "VCC 5 V   GND", fontsize=8, ha="center", color=RED)
    for i, (p, gpio, colour) in enumerate(TFT_ROWS):
        y = top - step * (i + .5)
        ax.text(tx + tw - 1, y, p, ha="right", va="center", fontsize=8.6, family="monospace")
        wire(ax, [(tx + tw, y), (ex, y)], colour)
        ax.text(ex + 1, y, f"GPIO{gpio}", ha="left", va="center", fontsize=8.6,
                family="monospace", color=colour, weight="bold")
    ax.text((tx + tw + ex) / 2, top + 1, "TFT bus", fontsize=8, color=BLUE, ha="center")
    ax.text((tx + tw + ex) / 2, top - step * 7, "touch + SD bus", fontsize=8, color=VIOLET, ha="center")

    # --- J1 cable connector, right ------------------------------------------
    cx, cw = 100, 26
    rows_y = [64 - 4.4 * i for i in range(6)]
    box(ax, cx, rows_y[-1] - 4, cw, rows_y[0] - rows_y[-1] + 10, "cable",
        "J1: cable to PROTOBOARD", size=9.2)
    for (n, sig, esp, _pb, colour), y in zip(J1, rows_y):
        to_esp = sig == "GPS TX"
        pts = [(cx, y), (ex + ew, y)] if to_esp else [(ex + ew, y), (cx, y)]
        wire(ax, pts, colour, arrow=sig.startswith("GPS"))
        ax.text(ex + ew - 1, y, esp, ha="right", va="center", fontsize=8.4,
                family="monospace", color=colour, weight="bold")
        pin(ax, cx + 2, y, "", side="left")
        ax.text(cx + 3.5, y, f"{n}  {sig}", ha="left", va="center", fontsize=8.6, family="monospace")
    ax.text((ex + ew + cx) / 2, rows_y[0] + 2.4, "6 wires", fontsize=8.5, ha="center", weight="bold")
    ax.text(cx + cw / 2, rows_y[-1] - 6.5, "Pinout matches J1 on the\nprotoboard drawing",
            fontsize=7.8, ha="center", va="top", color=GREY)

    # --- Antenna, bottom ---------------------------------------------------
    box(ax, 50, 1, 30, 7, "rf", None, "Router antenna (RP-SMA)\n+ pigtail to U.FL socket")
    wire(ax, [(65, 8), (65, 9.5)], GREY, width=1.4)

    # --- Notes, bottom right -----------------------------------------------
    box(ax, 86, 1, 42, 19, "note")
    ax.text(88, 18, "Before you power on", fontsize=9.5, weight="bold", va="top")
    ax.text(88, 14.8,
            "- Pull both JP1 caps on the protoboard to flash:\n"
            "  the GPS shares GPIO1/3 with the USB serial chip.\n"
            "- J1 pin 1 is 3V3, never the 5 V rail.\n"
            "- Wire and test one part at a time, on USB,\n"
            "  before the battery goes in.\n"
            "- Battery gauge: none (BATTERY_ADC_PIN -1).",
            fontsize=8.2, va="top")

    ax.text(2, 4.5, "Logical drawing: pins are grouped by part,\nnot in dev-board order. Wire by GPIO labels.",
            fontsize=8, color=GREY, va="center")
    save(fig, "wiring")


# ---------------------------------------------------------------------------
# 2. Protoboard
# ---------------------------------------------------------------------------
def draw_protoboard():
    fig, ax = plt.subplots(figsize=(13, 9.4))
    ax.set_xlim(0, 130); ax.set_ylim(0, 94); ax.axis("off")

    # Board outline
    box(ax, 6, 3, 122, 86, "pcb", lw=1.4)
    ax.text(8, 92.3, "PROTOBOARD: UNO proto shield (used on its own, not plugged into an UNO)",
            fontsize=12, weight="bold", va="center")

    # Rails
    V, G = 84, 80
    wire(ax, [(16, V), (124, V)], RED, width=2.6)
    wire(ax, [(19, G), (124, G)], BLACK, width=2.6)
    ax.text(124, V + 1.6, "3V3 rail  (shield prints it '5V': relabel it 3V3)", fontsize=8.2,
            color=RED, ha="right", weight="bold")
    ax.text(124, G - 1.9, "GND rail", fontsize=8.2, color=BLACK, ha="right", weight="bold")

    # --- J1 harness header, left edge ---------------------------------------
    jy = {1: 66, 2: 62, 3: 58, 4: 54, 5: 50, 6: 46}
    box(ax, 7.5, 42, 6.5, 28, "white")
    ax.text(10.7, 71.5, "J1", ha="center", fontsize=9.5, weight="bold")
    for n, sig, esp, _pb, colour in J1:
        pin(ax, 12.5, jy[n], str(n), side="right", fs=8)
        wire(ax, [(1.5, jy[n]), (7.5, jy[n])], colour, width=1.2)
    ax.text(3.5, 38.5, "6-wire cable\nto ESP32", fontsize=7.8, ha="center", va="top", color=GREY)
    ax.annotate("", xy=(1.2, 56), xytext=(5, 56),
                arrowprops=dict(arrowstyle="-|>", color=GREY, lw=1))

    # J1 power to rails
    wire(ax, [(13, jy[1]), (16, jy[1]), (16, V)], RED); dot(ax, 16, V, RED)
    wire(ax, [(13, jy[2]), (19, jy[2]), (19, G)], BLACK); dot(ax, 19, G, BLACK)

    # --- PCF8574 module -----------------------------------------------------
    px, pw, pyb, pyt = 34, 24, 41, 78
    box(ax, px, pyb, pw, pyt - pyb, "ui", "PCF8574T module", size=9.5)
    ax.text(px + pw / 2, pyt - 4.6, "addr jumpers: leave as-is\n(auto-detect 0x20-0x27)",
            fontsize=7.2, ha="center", va="top", color=GREY)
    left = {"VCC": 70, "GND": 66, "SDA": 58, "SCL": 54}
    for name, y in left.items():
        pin(ax, px, y, name, side="left")
    # SDA / SCL straight from J1
    wire(ax, [(13, jy[3]), (px, left["SDA"])], GREEN, "SDA", (24, 59.6), fs=7.8)
    wire(ax, [(13, jy[4]), (px, left["SCL"])], GREEN, "SCL", (24, 55.8), fs=7.8)
    # PCF power to rails
    wire(ax, [(px, left["VCC"]), (26, left["VCC"]), (26, V)], RED); dot(ax, 26, V, RED)
    wire(ax, [(px, left["GND"]), (29, left["GND"]), (29, G)], BLACK); dot(ax, 29, G, BLACK)

    # P0..P7 on the right edge
    p_y = {i: 72 - 4 * i for i in range(8)}  # P0 72 ... P7 44
    for i, y in p_y.items():
        name = BUTTON_OF.get(i)
        pin(ax, px + pw, y, f"P{i}", side="right",
            colour=BUTTON_COLOUR[name] if name else GREY)
        if not name:
            ax.text(px + pw + 1.5, y, "nc", fontsize=7.5, va="center", color=GREY)

    # --- Buttons (schematic column) ----------------------------------------
    bx, bus = 76, 88
    for i, name in BUTTON_OF.items():
        y = p_y[i]
        c = BUTTON_COLOUR[name]
        wire(ax, [(px + pw, y), (bx - 1.7, y)], c)
        ax.add_patch(Circle((bx, y), 1.7, fc="#333333", ec="#111111", zorder=6))
        ax.text(bx - 3, y + 1.25, name, fontsize=7.6, ha="right", va="bottom", color=c, weight="bold")
        wire(ax, [(bx + 1.7, y), (bus, y)], BLACK, width=1.2)
        dot(ax, bus, y, BLACK)
    ax.text(bx, p_y[7] - 3.2, "5 push buttons\n(other leg to GND)", fontsize=7.8, ha="center",
            va="top", color=GREY)
    # Local GND bus down from GND rail
    wire(ax, [(bus, G), (bus, 37)], BLACK, width=1.4); dot(ax, bus, G, BLACK)

    # --- Front-panel inset: how the buttons sit -----------------------------
    ix, iy, iw, ih = 96, 41, 29, 34
    box(ax, ix, iy, iw, ih, "white", "Front-panel layout", size=9, ls="--")
    cx, cy, d = ix + iw / 2, iy + 14, 7.5
    pos = {"UP": (cx, cy + d), "DOWN": (cx, cy - d), "LEFT": (cx - d, cy),
           "RIGHT": (cx + d, cy), "SELECT": (cx, cy)}
    p_of = {v: k for k, v in BUTTON_OF.items()}
    for name, (x, y) in pos.items():
        ax.add_patch(Circle((x, y), 2.4, fc="#333333", ec=BUTTON_COLOUR[name], lw=2, zorder=6))
        ax.text(x, y - 3.3, f"{name} P{p_of[name]}", fontsize=6.8, ha="center", va="top",
                color=BUTTON_COLOUR[name], weight="bold")
    ax.text(cx, iy + ih - 5, "Your photo shows 4: add a 5th\nin the centre for SELECT",
            fontsize=7.2, ha="center", va="top", color=RED)

    # --- GPS module ---------------------------------------------------------
    gx, gw, gyb, gyt = 34, 30, 7, 30
    box(ax, gx, gyb, gw, gyt - gyb, "rf")
    ax.text(gx + gw / 2, gyb + 10, "GY-NEO-7M GPS\n(patch antenna up,\nnothing metal above it)",
            ha="center", va="center", fontsize=8.6, weight="bold")
    # Header along the top edge, silkscreen order PPS RXD TXD GND VCC
    gpins = {"PPS": 40, "RXD": 45, "TXD": 50, "GND": 55, "VCC": 60}
    for name, x in gpins.items():
        ax.add_patch(FancyBboxPatch((x - .55, gyt - .55), 1.1, 1.1, boxstyle="square,pad=0",
                                    fc="#F0C040", ec="#333333", lw=.7, zorder=7))
        ax.text(x, gyt - 2, name, ha="center", va="top", fontsize=7.4, family="monospace",
                rotation=90)
    ax.text(gpins["PPS"], gyt + 1.6, "nc", fontsize=7.5, ha="center", color=GREY)
    # GPS power: VCC to 3V3 rail on the far right, GND onto the button GND bus
    wire(ax, [(gpins["VCC"], gyt), (gpins["VCC"], 34), (92, 34), (92, V)], RED,
         "VCC", (78, 34), fs=7.8)
    dot(ax, 92, V, RED)
    wire(ax, [(gpins["GND"], gyt), (gpins["GND"], 37), (bus, 37)], BLACK)
    dot(ax, bus, 37, BLACK)

    # --- JP1 flash jumpers between J1 and the GPS ----------------------------
    jx = 19
    box(ax, jx - 1, 43.6, 7, 8.8, "white")
    for y in (jy[5], jy[6]):
        pin(ax, jx, y, "", side="left"); pin(ax, jx + 5, y, "", side="left")
        ax.plot([jx, jx + 5], [y, y], color="#111111", lw=4.5, alpha=.35, zorder=8,
                solid_capstyle="butt")
    ax.text(jx + 2.5, 41.8, "JP1", fontsize=8.5, ha="center", va="top", weight="bold")
    ax.text(jx + 2.5, 39.6, "pull both caps\nto flash", fontsize=7.2, ha="center", va="top", color=RED)
    wire(ax, [(13, jy[5]), (jx, jy[5])], TEAL)
    wire(ax, [(13, jy[6]), (jx, jy[6])], TEAL)
    # GPS TXD -> JP1 -> J1 pin 5 (data toward the ESP32)
    wire(ax, [(gpins["TXD"], gyt), (gpins["TXD"], 40), (30, 40), (30, jy[5]), (jx + 5, jy[5])],
         TEAL, arrow=True)
    # J1 pin 6 -> JP1 -> GPS RXD
    wire(ax, [(jx + 5, jy[6]), (27, jy[6]), (27, 37.5), (gpins["RXD"], 37.5), (gpins["RXD"], gyt)],
         TEAL, arrow=True)
    ax.text(31, 41.4, "GPS TX", fontsize=7.6, color=TEAL)
    ax.text(28, 35.8, "GPS RX (optional)", fontsize=7.6, color=TEAL, va="top")

    # --- Notes --------------------------------------------------------------
    box(ax, 96, 6, 29, 30, "note")
    ax.text(98, 34, "Notes", fontsize=9.5, weight="bold", va="top")
    ax.text(98, 31,
            "- Everything here runs on\n"
            "  3V3 from the ESP32.\n"
            "- No fix on 3V3? Feed GPS\n"
            "  VCC from 5 V with a 7th\n"
            "  wire instead.\n"
            "- J1 pin 6 can be left out\n"
            "  (5-wire cable).\n"
            "- I2C pull-ups are on the\n"
            "  PCF8574 module already.",
            fontsize=7.8, va="top")
    save(fig, "wiring-protoboard")


# ---------------------------------------------------------------------------
# 3. Overview: the two assemblies and the cable
# ---------------------------------------------------------------------------
def draw_overview():
    fig, ax = plt.subplots(figsize=(13, 7.6))
    ax.set_xlim(0, 130); ax.set_ylim(0, 76); ax.axis("off")
    ax.text(2, 74.5, "HUB: the two assemblies and the J1 cable", fontsize=12, weight="bold", va="top")

    # Power chain feeding the left assembly
    box(ax, 2, 58, 18, 8, "power", None, "3x 18650\n(parallel)")
    box(ax, 24, 58, 13, 8, "ui", None, "Switch")
    box(ax, 41, 58, 18, 8, "power", None, "5V 2A\nboost module")
    wire(ax, [(20, 62), (24, 62)], RED, arrow=True)
    wire(ax, [(37, 62), (41, 62)], RED, arrow=True)
    wire(ax, [(50, 58), (50, 52)], RED, "5 V", (52.5, 55), arrow=True)

    # Left assembly
    box(ax, 2, 4, 58, 48, "compute", lw=1.6)
    ax.text(31, 49, "ESP32 + touchscreen assembly", ha="center", fontsize=11, weight="bold")
    ax.text(31, 46, "pin detail: wiring.svg", ha="center", fontsize=8, color=GREY)
    box(ax, 5, 9, 22, 32, "screen", '2.4" ILI9341\nTFT', None, size=9.5)
    ax.text(16, 25, "display\ntouch\nSD card", ha="center", va="center", fontsize=8.5)
    box(ax, 35, 9, 22, 32, "white", "ESP32-WROOM\n-32U", None, size=9.5)
    ax.text(46, 24, "U.FL antenna\nUSB for flashing", ha="center", va="center", fontsize=8.3,
            color=GREY)
    for i, (lbl, c) in enumerate([("TFT bus  15 0 2 13 14 12", BLUE), ("backlight  32", AMBER),
                                  ("touch+SD  18 23 19, CS 33 / 5", VIOLET)]):
        y = 34 - i * 5
        wire(ax, [(27, y), (35, y)], c, width=3)
    ax.text(31, 37.5, "12 wires", fontsize=7.8, ha="center", color=GREY)
    ax.text(31, 14, "+5 V\n+GND", fontsize=7.8, ha="center", color=RED)

    # Right assembly
    box(ax, 82, 4, 46, 62, "pcb", lw=1.6)
    ax.text(105, 63, "PROTOBOARD", ha="center", fontsize=11, weight="bold")
    ax.text(105, 60, "pin detail: wiring-protoboard.svg", ha="center", fontsize=8, color=GREY)
    box(ax, 96, 37, 28, 18, "ui", "PCF8574T", "I2C button expander\nP3-P7 to buttons", size=9.5)
    box(ax, 96, 23, 28, 11, "ui", None, "5 buttons\nUP DOWN LEFT RIGHT SELECT")
    wire(ax, [(110, 37), (110, 34)], GREY, width=3)
    box(ax, 96, 7, 28, 13, "rf", "GY-NEO-7M GPS", "UART, 9600 baud", size=9.5)
    box(ax, 86, 10, 6, 7, "white", None, "JP1")
    ax.text(89, 8.4, "flash\njumpers", fontsize=6.8, ha="center", va="top", color=RED)
    wire(ax, [(92, 13.5), (96, 13.5)], TEAL, width=2)

    # J1 cable: 6 wires from ESP32 to protoboard
    ys = [44, 41, 38, 35, 17, 14]
    for (n, sig, esp, _pb, c), y in zip(J1, ys):
        if n <= 4:
            pts = [(57, y), (96, y)]
        else:
            pts = [(57, y), (86, y)]
        if sig == "GPS TX":
            pts = pts[::-1]
        wire(ax, pts, c, width=2, arrow=sig.startswith("GPS"))
        ax.text(58, y + .9, f"{n}", fontsize=7.4, color=c, weight="bold")
        ax.text(68, y + .9, f"{esp}", fontsize=7.4, color=c, family="monospace")
    ax.text(74, 46.5, "J1: 6-wire cable", ha="center", fontsize=9, weight="bold")
    # rails into the PCF / GPS on the protoboard side
    ax.text(88, 44.8, "3V3", fontsize=7.4, color=RED)
    ax.text(88, 41.8, "GND", fontsize=7.4, color=BLACK)
    ax.text(88, 38.8, "SDA", fontsize=7.4, color=GREEN)
    ax.text(88, 35.8, "SCL", fontsize=7.4, color=GREEN)
    ax.text(110, 21.4, "3V3 + GND rails also feed the GPS", fontsize=7.4, ha="center",
            va="center", color=GREY)

    # Antenna
    box(ax, 63, 4, 16, 7, "rf", None, "Router antenna\nto U.FL")
    wire(ax, [(63, 7.5), (57, 7.5), (57, 9)], GREY, width=1.4)

    ax.text(66, 66, "The J1 cable carries:\n 1 3V3   2 GND\n 3 SDA   4 SCL\n 5 GPS TX   6 GPS RX",
            fontsize=8.2, va="top", family="monospace",
            bbox=dict(fc=FILL["note"], ec="#333333", lw=.8, boxstyle="round,pad=.5"))
    save(fig, "wiring-overview")


if __name__ == "__main__":
    draw_overview()
    draw_esp32_tft()
    draw_protoboard()
