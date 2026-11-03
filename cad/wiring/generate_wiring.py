#!/usr/bin/env python3
"""Draw the HUB wiring diagram for the ESP32-DIV v1 handheld build.

    pip install matplotlib
    python generate_wiring.py

Output:
    ../../docs/wiring.svg      the wiring drawing on the build page
    preview/wiring.png

Pins come from cifertech's ESP32-DIV firmware, v1 target (BOARD_ESP32_DIV_V1):
shared.h (touch, SD, backlight, PCF8574 buttons) and Libraries/User_Setup v1.h
(TFT bus). The drawing is logical: pins are grouped by part, not in the order
they sit on the dev board. Wire by the silkscreen GPIO labels.
"""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import FancyBboxPatch

HERE = Path(__file__).resolve().parent
DOCS = HERE.parent.parent / "docs"

FILL = {"screen": "#BFD7EA", "compute": "#CFCDE8", "rf": "#F2D7A6", "ui": "#E6D3EC",
        "power": "#F4C7B8", "note": "#F7F3E3"}
RED, AMBER, GREEN, BLUE, VIOLET, GREY = "#C8392B", "#B07A0E", "#1C8A6B", "#2B6CB0", "#6A4FC4", "#555555"

# (part pin label, ESP32 GPIO, colour). Rows run top to bottom; wires are straight.
TFT_ROWS = [
    ("CS", 15, BLUE), ("RESET", 0, BLUE), ("DC", 2, BLUE), ("SDI (MOSI)", 13, BLUE),
    ("SCK", 14, BLUE), ("LED", 32, AMBER), ("SDO (MISO)", 12, BLUE),
    ("T_CS", 33, VIOLET), ("T_CLK + SD_SCK", 18, VIOLET), ("T_DIN + SD_MOSI", 23, VIOLET),
    ("T_DO + SD_MISO", 19, VIOLET), ("SD_CS", 5, VIOLET),
]
BUTTONS = [("P6", "UP"), ("P3", "DOWN"), ("P4", "LEFT"), ("P5", "RIGHT"), ("P7", "SELECT")]


def box(ax, x, y, w, h, key, title=None, body=None, size=10):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=.8",
                                fc=FILL[key], ec="#333333", lw=1))
    if title:
        ax.text(x + w / 2, y + h - 2.2, title, ha="center", va="center", fontsize=size, weight="bold")
    if body:
        ax.text(x + w / 2, y + h / 2 - (1.6 if title else 0), body, ha="center", va="center", fontsize=8.8)


def wire(ax, pts, colour, label=None, at=None, width=1.6, arrow=False):
    p = np.array(pts, dtype=float)
    ax.plot(p[:, 0], p[:, 1], color=colour, lw=width, solid_capstyle="round")
    if arrow:
        ax.annotate("", xy=p[-1], xytext=p[-2], arrowprops=dict(arrowstyle="-|>", color=colour, lw=width))
    if label:
        ax.text(*at, label, fontsize=8.3, color=colour, ha="center", va="center",
                bbox=dict(fc="white", ec="none", pad=.5))


def draw_wiring():
    fig, ax = plt.subplots(figsize=(13, 9.4))
    ax.set_xlim(0, 130); ax.set_ylim(0, 94); ax.axis("off")

    # --- Power chain, top row ---------------------------------------------
    box(ax, 2, 84, 22, 8, "power", None, "3x 18650 2200 mAh\nin parallel (1S3P)")
    box(ax, 30, 84, 16, 8, "ui", None, "Master switch")
    box(ax, 52, 84, 26, 8, "power", None, "5V 2A charge/boost module\nBAT in, 5 V out")
    box(ax, 84, 84, 22, 8, "power", None, "USB-C breakout\n(charge input pads)")
    wire(ax, [(24, 88), (30, 88)], RED, arrow=True)
    wire(ax, [(46, 88), (52, 88)], RED, arrow=True)
    wire(ax, [(84, 88), (78, 88)], RED, arrow=True)
    ax.text(27, 90.6, "BAT+", fontsize=7.5, color=RED, ha="center")

    # 5 V rail: module out -> ESP32 VIN and TFT VCC; GPS taps it too.
    wire(ax, [(65, 84), (65, 79), (13, 79), (13, 77)], RED, "5 V rail", (40, 79))
    wire(ax, [(65, 79), (65, 72)], RED, "VIN", (67.5, 75.5), arrow=True)
    wire(ax, [(65, 79), (129, 79), (129, 37), (122, 37)], RED, "5 V to GPS VCC", (111, 79))

    # --- ESP32 -----------------------------------------------------------
    ex, ey, ew, eh = 50, 12, 30, 60
    box(ax, ex, ey, ew, eh, "compute")
    ax.text(ex + ew / 2, ey + eh - 3, "HUB: ESP32-WROOM-32U", ha="center", fontsize=10.5, weight="bold")
    ax.text(ex + ew / 2, ey + eh - 6.2, "dev board, ESP32-DIV v1 firmware", ha="center", fontsize=8.5)
    ax.text(ex + ew / 2, ey + 6, "3.3 V logic\nGND common to every part", ha="center", va="center",
            fontsize=8.5, color=GREY)

    # --- TFT: 2.4" ILI9341 + XPT2046 touch + SD, left ----------------------
    tx, tw = 2, 30
    top = 66
    step = 4.2
    box(ax, tx, top - step * len(TFT_ROWS) - 1.5, tw, step * len(TFT_ROWS) + 12.5, "screen",
        '2.4" ILI9341 TFT\ntouch + SD', size=9.5)
    ax.text(tx + tw / 2, top + 4.2, "VCC 5 V   GND", fontsize=8, ha="center", color=RED)
    for i, (pin, gpio, colour) in enumerate(TFT_ROWS):
        y = top - step * (i + .5)
        ax.text(tx + tw - 1, y, pin, ha="right", va="center", fontsize=8.6, family="monospace")
        wire(ax, [(tx + tw, y), (ex, y)], colour)
        ax.text(ex + 1, y, f"GPIO{gpio}", ha="left", va="center", fontsize=8.6,
                family="monospace", color=colour, weight="bold")
    ax.text((tx + tw + ex) / 2, top + 1, "TFT bus", fontsize=8, color=BLUE, ha="center")
    ax.text((tx + tw + ex) / 2, top - step * 7, "touch + SD bus", fontsize=8, color=VIOLET, ha="center")

    # --- PCF8574 + buttons, right ------------------------------------------
    box(ax, 90, 44, 20, 24, "ui", "PCF8574T", None, size=9.5)
    ax.text(91, 63, "VCC 3V3\nGND\n0x20-27", ha="left", va="top", fontsize=8)
    for y, (gpio, pin) in ((53, (21, "SDA")), (49, (22, "SCL"))):
        wire(ax, [(ex + ew, y), (90, y)], GREEN)
        ax.text(ex + ew - 1, y, f"GPIO{gpio}", ha="right", va="center", fontsize=8.6,
                family="monospace", color=GREEN, weight="bold")
        ax.text(91, y, pin, ha="left", va="center", fontsize=8.6, family="monospace")
    wire(ax, [(ex + ew, 62), (90, 62)], RED, "3V3 out", (85, 64))

    box(ax, 114, 44, 14, 24, "ui", "Buttons", None, size=9.5)
    for i, (p, name) in enumerate(BUTTONS):
        y = 61.5 - i * 3.0
        wire(ax, [(110, y), (114, y)], GREY, width=1.2)
        ax.text(109.5, y, p, ha="right", va="center", fontsize=8, family="monospace")
        ax.text(115, y, name, ha="left", va="center", fontsize=8.3)
    ax.text(121, 45.4, "other leg to GND", ha="center", fontsize=7.5, color=GREY)

    # --- GPS, right --------------------------------------------------------
    box(ax, 98, 24, 24, 16, "rf", "GY-NEO-7M GPS", None, size=9.5)
    ax.text(110, 36, "VCC 5 V   GND", ha="center", fontsize=8, color=RED)
    for y, gpio, pin, arrow_to_esp in ((32, 3, "TX", True), (27, 1, "RX", False)):
        pts = [(98, y), (ex + ew, y)] if arrow_to_esp else [(ex + ew, y), (98, y)]
        wire(ax, pts, GREEN, arrow=True)
        ax.text(ex + ew - 1, y, f"GPIO{gpio} {'RX' if gpio == 3 else 'TX'}", ha="right", va="center",
                fontsize=8.6, family="monospace", color=GREEN, weight="bold")
        ax.text(99, y, pin, ha="left", va="center", fontsize=8.6, family="monospace")
    ax.text(89, 29.5, "UART0 = USB serial", fontsize=7.8, color=GREEN, ha="center")

    # --- Antenna, bottom ---------------------------------------------------
    box(ax, 50, 1, 30, 7, "rf", None, "Router antenna (RP-SMA)\n+ pigtail to U.FL socket")
    wire(ax, [(65, 8), (65, 12)], GREY, width=1.4)

    # --- Notes, bottom right -----------------------------------------------
    box(ax, 86, 1, 42, 19, "note")
    ax.text(88, 18, "Before you power on", fontsize=9.5, weight="bold", va="top")
    ax.text(88, 14.8,
            "- Unplug the GPS to flash: it shares GPIO1/3\n"
            "  with the USB serial chip.\n"
            "- PCF8574 on 3V3, never the 5 V rail.\n"
            "- Wire and test one part at a time, on USB,\n"
            "  before the battery goes in.\n"
            "- Battery gauge: none (BATTERY_ADC_PIN -1).",
            fontsize=8.2, va="top")

    ax.text(2, 4.5, "Logical drawing: pins are grouped by part,\nnot in dev-board order. Wire by GPIO labels.",
            fontsize=8, color=GREY, va="center")

    fig.tight_layout()
    fig.savefig(DOCS / "wiring.svg")
    (HERE / "preview").mkdir(exist_ok=True)
    fig.savefig(HERE / "preview" / "wiring.png", dpi=90)
    plt.close(fig)


if __name__ == "__main__":
    draw_wiring()
