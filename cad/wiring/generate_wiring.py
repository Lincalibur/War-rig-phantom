#!/usr/bin/env python3
"""Draw the HUB wiring diagram for the ESP32-DIV v1 handheld build.

    pip install matplotlib
    python generate_wiring.py

The HUB is one assembly: an ESP32-WROOM-32U dev board with a 2.4" ILI9341
touch+SD TFT, a GY-NEO-7M GPS wired straight to the board, the power chain and
the antenna. There is no protoboard and no button matrix: the handheld is driven
from the CONSOLE phone app over BLE (and the touchscreen).

Output (in ../../docs/, PNG preview in preview/):
    wiring.svg    ESP32 + touchscreen + GPS, pin by pin

Pins come from cifertech's ESP32-DIV firmware, v1 target (BOARD_ESP32_DIV_V1):
shared.h (touch, SD, backlight, GPS UART) and Libraries/User_Setup v1.h (TFT bus).
The drawing is logical: wire by the silkscreen labels, not by where a pin sits in
the picture.
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

# GPS wired straight to the ESP32: (GPS pin, ESP32 end, colour). TXD carries data
# into the ESP32; RXD is optional (the GPS needs no commands). PPS is left unconnected.
GPS_ROWS = [
    ("VCC", "3V3", RED),
    ("GND", "GND", BLACK),
    ("TXD", "GPIO3 (RX0)", TEAL),
    ("RXD", "GPIO1 (TX0), optional", TEAL),
]


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


def save(fig, name):
    fig.tight_layout()
    fig.savefig(DOCS / f"{name}.svg")
    (HERE / "preview").mkdir(exist_ok=True)
    fig.savefig(HERE / "preview" / f"{name}.png", dpi=90)
    plt.close(fig)


# ---------------------------------------------------------------------------
# ESP32 + touchscreen + GPS
# ---------------------------------------------------------------------------
def draw_esp32_tft():
    fig, ax = plt.subplots(figsize=(13, 9.4))
    ax.set_xlim(0, 130); ax.set_ylim(0, 94); ax.axis("off")
    ax.text(2, 92.5, "HUB: ESP32 + touchscreen + GPS", fontsize=12, weight="bold", va="top")

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

    # --- GPS module, right (wired straight to the ESP32) -------------------
    gx, gw = 98, 28
    gtop = 54
    gstep = 5.0
    gbot = gtop - gstep * len(GPS_ROWS)
    box(ax, gx, gbot - 3, gw, gstep * len(GPS_ROWS) + 11, "rf")
    ax.text(gx + gw / 2, gtop + 4.0, "GY-NEO-7M GPS", fontsize=9.5, ha="center", weight="bold")
    ax.text(gx + gw / 2, gtop + 1.2, "patch antenna up", fontsize=7.8, ha="center", color=GREY)
    for i, (gp, esp, colour) in enumerate(GPS_ROWS):
        y = gtop - gstep * (i + .5)
        to_esp = gp == "TXD"
        pts = [(gx, y), (ex + ew, y)] if to_esp else [(ex + ew, y), (gx, y)]
        wire(ax, pts, colour, arrow=gp in ("TXD", "RXD"))
        ax.text(gx + 1.2, y, gp, ha="left", va="center", fontsize=8.6, family="monospace")
        ax.text(ex + ew - 1, y, esp, ha="right", va="center", fontsize=8.3,
                family="monospace", color=colour, weight="bold")
    ax.text((ex + ew + gx) / 2, gtop + 1.2, "UART, 9600 baud", fontsize=8, color=TEAL, ha="center")
    ax.text(gx + gw / 2, gbot - 4.2, "PPS pin: not connected", fontsize=7.6, ha="center",
            va="top", color=GREY)

    # --- Antenna, bottom ---------------------------------------------------
    box(ax, 50, 1, 30, 7, "rf", None, "Router antenna (RP-SMA)\n+ pigtail to U.FL socket")
    wire(ax, [(65, 8), (65, 9.5)], GREY, width=1.4)

    # --- Notes, bottom right -----------------------------------------------
    box(ax, 86, 1, 42, 21, "note")
    ax.text(88, 20, "Before you power on", fontsize=9.5, weight="bold", va="top")
    ax.text(88, 16.8,
            "- Unplug the GPS (or pull its TXD lead off\n"
            "  GPIO3) to flash: the GPS shares GPIO1/3\n"
            "  with the USB serial chip.\n"
            "- The GPS runs on 3V3. No fix on 3V3? Feed\n"
            "  its VCC from 5 V instead, GND stays common.\n"
            "- No buttons: the menu is driven by touch and\n"
            "  the CONSOLE phone app over BLE.\n"
            "- Battery gauge: none (BATTERY_ADC_PIN -1).",
            fontsize=8.2, va="top")

    ax.text(2, 4.5, "Logical drawing: pins are grouped by part,\nnot in dev-board order. Wire by GPIO labels.",
            fontsize=8, color=GREY, va="center")
    save(fig, "wiring")


if __name__ == "__main__":
    draw_esp32_tft()
