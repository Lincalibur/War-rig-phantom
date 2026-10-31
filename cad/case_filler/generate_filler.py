#!/usr/bin/env python3
"""Generate the 3D-printable filler for the clamshell case (Raspberry Pi 5 build).

    pip install trimesh manifold3d shapely matplotlib
    python generate_filler.py

Each half of the case gets four blocks that lock together with vertical
dovetails. Output:
    stl/*.stl                          the 8 print files
    preview/*.png                      slices through each half
    preview/all_blocks_VIEW_ONLY.stl   all blocks laid out as the open case
    ../../docs/case-layout.svg         the placement drawing on the build page
    ../../docs/wiring.svg              the wiring drawing on the build page

Coordinates, in mm, seen looking down into an open half:
    x = 0 at the left wall, y = 0 at the hinge, z = 0 on the case floor.
The lid folds about the hinge, so a lid part at (x, y) lands on the base part
at the same (x, y) when the case closes.

Change the numbers in the next section and re-run. Everything else follows.
"""
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import trimesh
from manifold3d import CrossSection, FillRule, Manifold, OpType
from matplotlib.patches import FancyBboxPatch, Polygon as PatchPolygon, Rectangle
from shapely.geometry import Polygon
from shapely.geometry.polygon import orient

# --- Case interior (measure these) -----------------------------------------
INNER_W = 205.0        # inside width at the rim. 220 is the OUTSIDE width.
DEPTH = 25.0           # floor to rim
Y_CORNER = 190.0       # hinge wall to front wall, at the side walls
Y_FLANK = 210.0        # hinge wall to front wall, next to the handle
Y_HANDLE = 165.0       # hinge wall to the handle cut-out
HANDLE_W = 100.0       # handle box width at the cut-out, including its walls
HANDLE_W_FRONT = 116.0 # handle box width where it meets the front wall
CLEARANCE = 0.5        # gap between filler and case, per side
CORNER_R = 12.0
FLOOR_CHAMFER = 3.0    # clears the radius where the walls meet the floor
BOSS_XY = (INNER_W / 2 - CLEARANCE, 103.0)  # moulded centre boss
BOSS_D, BOSS_H = 26.0, 8.0                  # relief for it under the filler

# --- Joints ----------------------------------------------------------------
SEAM_X = 102.0         # left/right split
SEAM_Y = 119.0         # hinge/front split
DOVE_ROOT, DOVE_TIP, DOVE_LEN = 14.0, 18.0, 5.0
DOVE_FIT = 0.2         # slot clearance per side, for an outsourced FDM print
DOVES_X = (30.0, 85.0, 145.0)  # y positions on the left/right seam
DOVES_Y = (30.0, 75.0)         # x positions on the left hinge/front seam
DOVE_Y_RIGHT = 113.0           # x position on the right hinge/front seam

# --- Parts -----------------------------------------------------------------
PANEL_RECESS = 5.0     # 7" panel thickness: MEASURE the real panel
PANEL = (19.0, 185.0, 14.0, 116.0)  # x0, x1, y0, y1: 7" panel, about 165 x 100
SWITCH_HOLE = 6.2      # SMTS-203 mini toggle
MASTER_HOLE = 12.2     # spare master switch
DC_HOLE = 8.2          # bulkhead DC socket, M8 thread per the nde3d listing
PI_HOLE = 2.8          # M2.5 standoff, Pi hangs under the base deck
SKIN = 3.0             # top skin over the lid screens
TOGGLE_ROOM = 7.0      # lid recess the base toggles rise into

HERE = Path(__file__).resolve().parent
DOCS = HERE.parent.parent / "docs"
PANEL_Z = DEPTH - PANEL_RECESS


def box(x0, x1, y0, y1, z0=-1.0, z1=DEPTH + 1):
    return Manifold.cube((x1 - x0, y1 - y0, z1 - z0)).translate((x0, y0, z0))


def cyl(x, y, d, z0=-1.0, z1=DEPTH + 1):
    return Manifold.cylinder(z1 - z0, d / 2, d / 2, 64).translate((x, y, z0))


def extrude(poly, z0, z1):
    rings = [np.array(orient(poly, 1.0).exterior.coords[:-1])]
    return CrossSection(rings, FillRule.Positive).extrude(z1 - z0).translate((0, 0, z0))


def outline():
    c = INNER_W / 2 - CLEARANCE
    w, yc, yf, yh = 2 * c, Y_CORNER - CLEARANCE, Y_FLANK - CLEARANCE, Y_HANDLE - CLEARANCE
    hn, hf = HANDLE_W / 2 + CLEARANCE, HANDLE_W_FRONT / 2 + CLEARANCE
    p = Polygon([(0, 0), (w, 0), (w, yc), (c + hf, yf), (c + hn, yh),
                 (c - hn, yh), (c - hf, yf), (0, yc)])
    return p.buffer(-CORNER_R, quad_segs=16).buffer(CORNER_R, quad_segs=16)


def filler_body():
    """Filler solid with the bottom outer edge chamfered."""
    shape = outline()
    hull = shape.convex_hull
    slab = lambda poly, z: extrude(poly, z, z + 0.01)
    tool = Manifold.batch_hull([
        slab(hull.buffer(-FLOOR_CHAMFER), 0),
        slab(hull, FLOOR_CHAMFER),
        slab(hull, DEPTH - 0.01),
    ])
    return extrude(shape, 0, DEPTH) ^ tool


def shared_cuts(screw_head_z):
    """Features common to both halves: boss, clamp screw, hinge cable path."""
    x, y = BOSS_XY
    return [
        cyl(x, y, BOSS_D, -1, BOSS_H),
        cyl(x, y, 3.4),
        cyl(x, y, 8.0, screw_head_z, DEPTH + 1),
        box(90, 114, -1, 10),                # hinge bundle passes the rim here
        box(6, 198, 8, 20, -1, 12),          # wiring trench along the hinge
    ]


def lid_top_cuts():
    """Cut last, so they also trim the dovetails that pass under the panel."""
    x0, x1, y0, y1 = PANEL
    return [box(x0, x1, y0, y1, PANEL_Z, DEPTH + 1)]


def lid_cuts():
    return shared_cuts(PANEL_Z - 3) + [
        # Behind the panel: driver board left; buck, boost and both radios right
        box(23, 97, 22, 112),
        box(109, 181, 22, 112),
        # Antenna pigtails run from the right bay to holes drilled in the case wall
        box(180, 210, 40, 52, 3, 14),
        box(180, 210, 70, 82, 3, 14),
        # STATUS OLED and BLE-SCAN: fit from behind, screen shows through skin
        box(7, 42, 124, 160, -1, DEPTH - SKIN),
        box(12, 40, 134, 150),
        box(44, 99, 122, 160, -1, DEPTH - SKIN),
        box(49, 94, 130, 154),
        # Recess for the base toggles, GPS below it with the patch facing the
        # lid shell (outward; the panel's metal back would block it otherwise)
        box(123, 198, 124, 156, DEPTH - TOGGLE_ROOM, DEPTH + 1),
        box(146, 192, 126, 154, -1, DEPTH - TOGGLE_ROOM - 2),
        # Alerts: piezo left of the handle; LEDs and mute button right of it
        box(12, 36, 166, 186, -1, DEPTH - SKIN),
        cyl(24, 176, 12.2),
        box(158, 196, 166, 186, -1, DEPTH - SKIN),
        cyl(165, 176, 5.2), cyl(175, 176, 5.2), cyl(187, 176, 12.2),
        # Wire links between pockets
        box(25, 37, 19, 23, -1, 10),
        box(96, 110, 62, 74, -1, 10),
        box(44, 56, 111, 125, -1, 10),
        box(41, 45, 135, 145, -1, 10),
        box(150, 165, 111, 127, -1, 10),
        box(170, 180, 153, 167, -1, 10),
        box(20, 28, 159, 167, -1, 10),
    ]


def base_cuts():
    holes = [cyl(x, y, SWITCH_HOLE) for x in (130, 150, 170, 190) for y in (130, 148)]
    holes[-1] = cyl(190, 148, MASTER_HOLE)
    vents = [cyl(x, y, 4.0) for x in np.arange(19, 62, 7.0) for y in np.arange(44, 90, 7.0)]
    pi_mounts = [cyl(x, y, PI_HOLE) for x in (15.5, 64.5) for y in (27.5, 85.5)]
    return shared_cuts(DEPTH - 3) + holes + vents + pi_mounts + [
        # Pi 5 hangs under the deck, cooler up against the vents. BMS beside it.
        box(8, 98, 22, 112, -1, 23),
        # 4-cell holder
        box(118, 198, 22, 102, -1, 23),
        # Fuse holder flat in a top groove, cap reachable with the lid open
        box(125, 173, 104, 118, 12, DEPTH + 1),
        # DC charge socket
        box(176, 198, 104, 118, -1, 23),
        cyl(186, 111, DC_HOLE),
        # Sunken switch panel, wiring space below it
        box(123, 198, 124, 156, 20, DEPTH + 1),
        box(123, 198, 124, 156, -1, 17),
        # 5 A buck (66 x 39 x 18), with the XL4015 in the flank
        box(8, 100.5, 122, 162, -1, 23),
        box(8, 34, 160, 186, -1, 23),
        # Battery meter
        box(158, 196, 166, 186, -1, 23),
        box(160, 194, 170, 182),
        # Wire links between pockets
        box(20, 32, 19, 23, -1, 10),
        box(130, 136, 101, 106, -1, 13),
        box(150, 165, 19, 23, -1, 10),
        box(150, 165, 101, 125, -1, 10),
        box(172, 178, 101, 106, -1, 10),
        box(97, 119, 62, 74, -1, 10),
        box(44, 56, 111, 123, -1, 10),
        box(170, 180, 155, 167, -1, 10),
    ]


def dovetail(root_axis_pos, seam, direction, along_x, grow=0.0):
    """Trapezoid at `seam`, pointing `direction` (+1/-1) across it."""
    r, t, l = DOVE_ROOT / 2 + grow, DOVE_TIP / 2 + grow, DOVE_LEN + grow
    back = 0.5 if grow == 0 else 0.0  # overlap so the male fuses to its block
    pts = [(-back, r), (l, t), (l, -t), (-back, -r)]
    if along_x:  # pointing along +-x, centred on y = root_axis_pos
        ring = [(seam + direction * a, root_axis_pos + b) for a, b in pts]
    else:        # pointing along +-y, centred on x = root_axis_pos
        ring = [(root_axis_pos + b, seam + direction * a) for a, b in pts]
    return extrude(Polygon(ring), -1, DEPTH + 1)


def blocks(cuts, top_cuts=()):
    body = filler_body() - Manifold.batch_boolean(cuts, OpType.Add)
    big = 400
    cells = {
        "hinge_left": box(-big, SEAM_X, -big, SEAM_Y),
        "hinge_right": box(SEAM_X, big, -big, SEAM_Y),
        "front_left": box(-big, SEAM_X, SEAM_Y, big),
        "front_right": box(SEAM_X, big, SEAM_Y, big),
    }
    parts = {k: body ^ v for k, v in cells.items()}

    joints = []  # (male block, female block, shape, slot)
    for y in DOVES_X:
        owner = ("hinge_left", "hinge_right") if y < SEAM_Y else ("front_left", "front_right")
        joints.append((*owner, dovetail(y, SEAM_X, 1, True), dovetail(y, SEAM_X, 1, True, DOVE_FIT)))
    for x in DOVES_Y:
        joints.append(("front_left", "hinge_left", dovetail(x, SEAM_Y, -1, False),
                       dovetail(x, SEAM_Y, -1, False, DOVE_FIT)))
    joints.append(("hinge_right", "front_right", dovetail(DOVE_Y_RIGHT, SEAM_Y, 1, False),
                   dovetail(DOVE_Y_RIGHT, SEAM_Y, 1, False, DOVE_FIT)))

    for male, female, shape, slot in joints:
        parts[female] = parts[female] - slot
        parts[male] = parts[male] + (shape ^ box(-big, big, -big, big, 0, DEPTH))
    for cut in top_cuts:
        parts = {k: p - cut for k, p in parts.items()}
    return parts


def to_trimesh(m):
    mesh = m.to_mesh()
    return trimesh.Trimesh(vertices=np.asarray(mesh.vert_properties)[:, :3],
                           faces=np.asarray(mesh.tri_verts), process=True)


def plot_slices(parts, name, z_levels):
    fig, axes = plt.subplots(1, len(z_levels), figsize=(7 * len(z_levels), 7.5))
    colours = dict(zip(parts, ("#4C78A8", "#F58518", "#54A24B", "#B279A2")))
    for ax, z in zip(np.atleast_1d(axes), z_levels):
        for key, part in parts.items():
            polys = part.slice(z).to_polygons()
            for ring in polys:
                r = np.vstack([ring, ring[0]])
                ax.plot(r[:, 0], r[:, 1], color=colours[key], lw=.8)
        ax.set_title(f"{name}: slice at z = {z} mm (hinge at the bottom)")
        ax.set_aspect("equal"); ax.grid(alpha=.25); ax.set_xlabel("x mm"); ax.set_ylabel("y mm")
    fig.tight_layout()
    fig.savefig(HERE / "preview" / f"{name}_slices.png", dpi=110)
    plt.close(fig)


# (x0, x1, y0, y1, label, colour key)
LID_ZONES = [
    (19, 185, 14, 116, '7" PANEL  (screen side up)\nBehind it, left: RTD2660 driver board\nBehind it, right: lid 5 V buck, 12 V boost,\nWIFI-NODE, CC1101 (antennas out the side wall)', "screen"),
    (7, 42, 124, 160, "STATUS\nC3 + OLED", "node"),
    (44, 99, 122, 160, "BLE-SCAN\nESP32 + 1.9\"", "node"),
    (146, 192, 126, 154, "GPS\nNEO-7M\npatch faces lid shell", "rf"),
    (12, 36, 166, 186, "Piezo", "ui"),
    (158, 196, 166, 186, "LEDs + mute", "ui"),
]
BASE_ZONES = [
    (12, 68, 24, 109, "RASPBERRY PI 5\nhangs under deck\ncooler up to vents", "compute"),
    (70, 98, 66, 112, "BMS", "power"),
    (118, 198, 22, 102, "18650 x4\n2S2P holder", "power"),
    (125, 173, 104, 118, "Fuse (flat in groove)", "power"),
    (176, 198, 104, 118, "DC in", "power"),
    (123, 198, 124, 156, "6 section toggles + master", "ui"),
    (34, 100, 122, 161, "5 A buck for the Pi\n66 x 39 x 18", "power"),
    (8, 34, 124, 186, "XL4015\ncharger", "power"),
    (158, 196, 166, 186, "Battery meter", "ui"),
]
ZONE_COLOURS = {"screen": "#BFD7EA", "node": "#C9E4CA", "rf": "#F2D7A6", "ui": "#E6D3EC",
                "power": "#F4C7B8", "compute": "#CFCDE8"}


def draw_layout():
    """Placement drawing: same coordinates as the STLs."""
    fig, axes = plt.subplots(1, 2, figsize=(13, 7.2))
    shape = np.array(outline().exterior.coords)
    for ax, title, zones in ((axes[0], "LID, looking at the screen side", LID_ZONES),
                             (axes[1], "BASE, looking down at the deck", BASE_ZONES)):
        ax.add_patch(PatchPolygon(shape, closed=True, fc="#F2F2EF", ec="#333333", lw=1.4))
        ax.plot([SEAM_X, SEAM_X], [0, Y_HANDLE], color="#888888", ls="--", lw=.8)
        ax.plot([0, INNER_W - 1], [SEAM_Y, SEAM_Y], color="#888888", ls="--", lw=.8)
        for x0, x1, y0, y1, label, key in zones:
            ax.add_patch(Rectangle((x0, y0), x1 - x0, y1 - y0, fc=ZONE_COLOURS[key], ec="#444444", lw=.8))
            ax.text((x0 + x1) / 2, (y0 + y1) / 2, label, ha="center", va="center", fontsize=8.5)
        ax.add_patch(Rectangle((90, 0), 24, 10, fc="white", ec="#444444", lw=.8))
        ax.text(102, -7, "HINGE: cable bundle crosses here", ha="center", fontsize=8.5)
        ax.text(102, 187, "handle", ha="center", fontsize=8.5, color="#666666")
        ax.set_xlim(-6, 210); ax.set_ylim(-14, 214); ax.set_aspect("equal"); ax.axis("off")
        ax.set_title(title, fontsize=12)
    fig.text(.5, .015, "Dimensions in mm, nominal. Dashed lines: seams between the printed blocks. "
                       "Each half is 25 mm deep.", ha="center", fontsize=9, color="#555555")
    fig.tight_layout(rect=(0, .03, 1, .97))
    fig.savefig(DOCS / "case-layout.svg")
    fig.savefig(HERE / "preview" / "case-layout.png", dpi=90)
    plt.close(fig)


def draw_wiring():
    fig, ax = plt.subplots(figsize=(13, 8.6))
    ax.set_xlim(0, 130); ax.set_ylim(0, 86); ax.axis("off")
    NODES = {
        "ADAPTER": (2, 76, 17, 7, "12 V adapter", "power"),
        "XL": (24, 76, 17, 7, "XL4015\nset 8.4 V, 1 A", "power"),
        "PACK": (46, 76, 17, 7, "18650 2S2P pack", "power"),
        "BMS": (68, 76, 15, 7, "2S BMS", "power"),
        "FUSE": (88, 76, 13, 7, "8 A fuse", "power"),
        "MASTER": (106, 76, 20, 7, "Master switch", "ui"),
        "BUCKA": (72, 60, 20, 8, "5 A buck, base\nset 5.1 V", "power"),
        "BUCKB": (106, 60, 20, 8, "XL4005 buck, lid\nset 5.1 V", "power"),
        "BOOST": (46, 60, 20, 8, "Boost, lid\nset 12 V", "power"),
        "PI": (40, 24, 46, 24, "", "compute"),
        "DRIVER": (4, 42, 24, 11, "RTD2660 driver board", "screen"),
        "PANEL": (4, 26, 24, 10, '7" panel', "screen"),
        "WIFI": (100, 42, 26, 10, "WIFI-NODE\nESP32-C3", "node"),
        "BLE": (100, 26, 26, 10, "BLE-SCAN\nESP32 + 1.9\"", "node"),
        "GPS": (100, 10, 26, 10, "GPS NEO-7M", "rf"),
        "CC": (4, 8, 24, 10, "CC1101 433 MHz", "rf"),
        "ALERTS": (40, 6, 46, 10, "Piezo, LED, mute button", "ui"),
        "STATUS": (104, 53.5, 22, 5, "STATUS  C3 + OLED", "node"),
    }
    for name, (x, y, w, h, label, key) in NODES.items():
        if not w:
            continue
        ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=.8",
                                    fc=ZONE_COLOURS[key], ec="#333333", lw=1))
        if label:
            ax.text(x + w / 2, y + h / 2, label, ha="center", va="center", fontsize=10)
    ax.text(63, 44.5, "RASPBERRY PI 5  (HUB, in the base)", ha="center", va="center", fontsize=11, weight="bold")
    ax.text(63, 33, "GPIO header, 3.3 V logic\nuart0 GPS   uart2 WIFI-NODE   uart4 BLE-SCAN\nSPI0 CC1101   GPIO alerts\nmicroSD holds the logs",
            ha="center", va="center", fontsize=9.5)

    def wire(pts, colour, label=None, at=None, width=1.8):
        p = np.array(pts)
        ax.plot(p[:, 0], p[:, 1], color=colour, lw=width, solid_capstyle="round")
        ax.annotate("", xy=p[-1], xytext=p[-2], arrowprops=dict(arrowstyle="-|>", color=colour, lw=width))
        if label:
            ax.text(*at, label, fontsize=8.5, color=colour, ha="center", va="center",
                    bbox=dict(fc="white", ec="none", pad=.6))

    red, amber, green, blue, violet = "#C8392B", "#B07A0E", "#1C8A6B", "#2B6CB0", "#6A4FC4"
    wire([(19, 79.5), (24, 79.5)], red)
    wire([(41, 79.5), (46, 79.5)], red)
    wire([(63, 79.5), (68, 79.5)], red)
    wire([(83, 79.5), (88, 79.5)], red)
    wire([(101, 79.5), (106, 79.5)], red)
    wire([(116, 76), (116, 68)], red, "7.4 V", (119.5, 72))
    wire([(116, 73), (82, 73), (82, 68)], red)
    wire([(116, 73), (56, 73), (56, 68)], red)
    wire([(82, 60), (82, 48)], red, "Pi 5 V: pins 2, 4", (73, 54))
    wire([(116, 60), (116, 58.5)], red, "switches 3-5: 5 V to all three nodes", (94, 58))
    wire([(56, 60), (56, 58), (16, 58), (16, 53)], amber, "12 V", (34, 58))
    wire([(11, 42), (11, 36)], violet, "50-pin FPC", (11, 39.2))
    wire([(40, 42), (28, 42)], violet, "micro-HDMI across the hinge", (34, 45.5))
    wire([(100, 44), (86, 44)], green, "GPIO5 RX / GPIO4 TX", (93, 40.5), 1.6)
    wire([(100, 31), (86, 31)], green, "GPIO13 RX / GPIO12 TX", (93, 28.2), 1.6)
    wire([(100, 15), (93, 15), (93, 22), (84, 22), (84, 24)], green, "GPIO15 RX", (92, 18.5), 1.6)
    wire([(112, 20), (112, 26)], green, "GPS TX", (116.5, 23), 1.6)
    wire([(126, 15), (128, 15), (128, 47), (126, 47)], green, None, None, 1.6)
    wire([(40, 28), (22, 28), (22, 18)], blue, "SPI0 + GDO", (30, 25.5), 1.6)
    wire([(63, 24), (63, 16)], violet, "GPIO 18, 22, 23", (70, 20), 1.6)
    fig.tight_layout()
    fig.savefig(DOCS / "wiring.svg")
    fig.savefig(HERE / "preview" / "wiring.png", dpi=90)
    plt.close(fig)


def view_position(mesh, half, key, spread=4.0):
    """Place a block as in the open case: lid above the hinge, base below it."""
    v = mesh.vertices.copy()
    v[:, 0] += spread if key.endswith("right") else -spread
    v[:, 1] += 2 * spread if key.startswith("front") else spread
    if half == "lid":
        return trimesh.Trimesh(v, mesh.faces, process=False)
    v[:, 1] *= -1  # the base folds out from the hinge, so mirror it
    return trimesh.Trimesh(v, mesh.faces[:, ::-1], process=False)


def main():
    (HERE / "stl").mkdir(exist_ok=True)
    (HERE / "preview").mkdir(exist_ok=True)
    total, ok, view_parts = 0.0, True, []
    print(f"{'file':<28}{'size mm':<24}{'cm3':>7}  watertight bodies")
    for half, cuts, top in (("lid", lid_cuts(), lid_top_cuts()), ("base", base_cuts(), ())):
        parts = blocks(cuts, top)
        for key, part in parts.items():
            mesh = to_trimesh(part)
            mesh.export(HERE / "stl" / f"{half}_{key}.stl")
            ext = mesh.extents
            cm3 = mesh.volume / 1000
            total += cm3
            bodies = mesh.body_count
            ok &= mesh.is_watertight and bodies == 1
            print(f"{half + '_' + key + '.stl':<28}{ext[0]:6.1f} x{ext[1]:6.1f} x{ext[2]:5.1f}   "
                  f"{cm3:7.1f}  {mesh.is_watertight!s:<10} {bodies}")
            view_parts.append(view_position(mesh, half, key))
        plot_slices(parts, half, (22, 8))
    trimesh.util.concatenate(view_parts).export(HERE / "preview" / "all_blocks_VIEW_ONLY.stl")
    draw_layout()
    draw_wiring()
    print(f"Solid-equivalent volume of all 8 blocks: {total:.0f} cm3")
    if not ok:
        raise SystemExit("A block is not a single watertight body")


if __name__ == "__main__":
    main()
