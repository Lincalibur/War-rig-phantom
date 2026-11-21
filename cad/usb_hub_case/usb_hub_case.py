"""
Parametric 3D-printable case for the 7-port USB hub PCB.

The board stands on its long edge. The USB ports point FORWARD through the front wall, and the
toggle switches point UP through the lid. Each switch sits between two ports, in front of the
board, and cuts one port's 5 V (VBUS).

Two printed parts:
  body  open-top box: front wall with port openings + LED window, back-wall standoffs for the board,
        screw bosses, cable slot in the end wall. Prints open side up.
  lid   flat plate: 7 switch holes with ON marks, 3 countersunk M2 screw holes, a rib underneath
        that holds the board's top edge. Prints outer face down.

    python cad/usb_hub_case/usb_hub_case.py      -> out/usb_hub_body.stl, out/usb_hub_lid.stl
    python cad/usb_hub_case/render_preview.py    -> out/preview*.png

Every dimension marked EST was scaled off a hand-held photo (hardware/Usb Hub.jpg).
Measure and edit before printing; see README.md. The script refuses to build a layout that collides.

Units mm. Case axes: X = along the hub (port-1 end -> cable end), Y = depth (0 = front face), Z = up.
Board coordinates: bx = along the board from the port-1 edge, bz = across the board from its bottom edge.
"""
from pathlib import Path
import numpy as np
import trimesh
from manifold3d import Manifold

# ----------------------------------------------------------------------------- board (all EST)
PCB_L, PCB_W, PCB_T = 130.0, 29.0, 1.6
PORT_BX = [2.5, 17.9, 33.3, 48.7, 64.1, 79.5, 94.9]  # port centres along the board
PORT_BZ = PCB_W / 2                                 # port centre across the board
PORT_SHELL_X, PORT_SHELL_Z = 7.2, 14.4              # port shell outside: along x across the board
PORT_CLEAR = 0.3                                    # gap around each shell in the front-wall opening
PORT_H = 13.0                                       # port face distance from the board's front surface
BACK_PARTS = 3.0                                    # pins / solder / chips on the board's back
HOLES = [(9, 2.6), (9, PCB_W - 2.6), (61, 2.6), (61, PCB_W - 2.6), (112, 2.6), (112, PCB_W - 2.6)]
SCREW_PILOT, PIN_D = 1.8, 1.8                       # M2 self-tap pilot; locating pin for the top holes
LED_BX, LED_BZ, LED_D = 106.0, PCB_W / 2, 3.4
CABLE_BZ, CABLE_W = 20.0, 5.5                       # where the input cable leaves the board end
END_PART_X = 3.0                                    # black part overhangs the cable end by this

# ----------------------------------------------------------------------------- switches (EST)
# Sub-miniature toggle. Body narrow side along X (fits the gap between port shells), long side
# front-to-back. Switch n sits on the port-1 side of port n: switch 1 is just past the board's
# port-1 end, switches 2-7 sit in the gaps between ports. That keeps the LED clear.
SW_HOLE_D = 5.2                                     # M5 bushing; 6.5 for 1/4", 6.3 for 6 mm
SW_BODY_X, SW_BODY_Y = 7.0, 12.5
SW_DEPTH = 11.0                                     # body + lugs below the lid
SW_LEVER_H = 9.0

# ----------------------------------------------------------------------------- case
FRONT_WALL, WALL, FLOOR, LID_T = 1.6, 2.0, 2.0, 2.0
GAP_PORT_END, GAP_CABLE_END = 10.0, 12.0            # room past each board end
GAP_TOP = 0.5                                       # board top edge to lid
BACK_GAP = 2.0                                      # air behind BACK_PARTS
STANDOFF_D = 5.0
BOSS_D, BOSS_PILOT, LID_SCREW_D, CSK_D = 6.0, 1.8, 2.4, 4.4
DRIVER_HOLE_D = 3.2                                 # front-wall access for the screwdriver (bottom screws)
RIB_T, RIB_H = 1.2, 3.0                             # lid rib behind the board's top edge

# derived positions (case coordinates)
PCB_FRONT_Y = max(FRONT_WALL + SW_BODY_Y + 0.3, FRONT_WALL + PORT_H - FRONT_WALL)  # board front face
PORT_FACE_Y = PCB_FRONT_Y - PORT_H                  # where the port openings end (0 = flush with the front)
PCB_BACK_Y = PCB_FRONT_Y + PCB_T
IN_BACK_Y = PCB_BACK_Y + BACK_PARTS + BACK_GAP
OUT_D = IN_BACK_Y + WALL
IN_L = GAP_PORT_END + PCB_L + GAP_CABLE_END
OUT_L = IN_L + 2 * WALL
PCB_Z0 = FLOOR                                      # board stands on the floor
LID_UNDER = PCB_Z0 + PCB_W + GAP_TOP
OUT_H = LID_UNDER + LID_T
cx = lambda bx: WALL + GAP_PORT_END + bx            # board -> case coordinates
cz = lambda bz: PCB_Z0 + bz
PITCH = [b - a for a, b in zip(PORT_BX, PORT_BX[1:])]
SW_BX = [PORT_BX[0] - PITCH[0] / 2] + [(a + b) / 2 for a, b in zip(PORT_BX, PORT_BX[1:])]
SW_Y = FRONT_WALL + SW_BODY_Y / 2
SW_BOTTOM_Z = LID_UNDER - SW_DEPTH
BOSS_BACK_Y = IN_BACK_Y - BOSS_D / 2
BOSSES = [(cx(-GAP_PORT_END / 2), BOSS_BACK_Y),
          (cx(PCB_L + END_PART_X + (GAP_CABLE_END - END_PART_X) / 2), BOSS_BACK_Y),
          (cx(PCB_L + END_PART_X + (GAP_CABLE_END - END_PART_X) / 2), FRONT_WALL + BOSS_D / 2)]

SEG = 48
def box(x, y, z, sx, sy, sz): return Manifold.cube([sx, sy, sz]).translate([x, y, z])
def cyl(x, y, z, h, d, d2=None):
    return Manifold.cylinder(h, d / 2, (d2 if d2 is not None else d) / 2, SEG).translate([x, y, z])
def ycyl(x, y0, z, length, d):
    """Cylinder along +Y from y0, centred on (x, z)."""
    return Manifold.cylinder(length, d / 2, d / 2, SEG).rotate([-90, 0, 0]).translate([x, y0, z])
def teardrop_y(x, y0, z, length, d):
    """Horizontal standoff that prints without support: a cylinder hulled with a 45-degree point below."""
    r = d / 2
    tip = box(x - 0.05, y0, z - r * 1.414 - 0.05, 0.1, length, 0.1)
    return Manifold.batch_hull([ycyl(x, y0, z, length, d), tip])


def layout_checks():
    problems = []
    def rect(x0, x1, a0, a1): return (x0, x1, a0, a1)
    def gap(a, b): return max(b[0] - a[1], a[0] - b[1], b[2] - a[3], a[2] - b[3])
    # plan view (x, y): switch bodies vs bosses; elevation (x, z): switch bodies vs port shells
    sw_xy = [rect(cx(b) - SW_BODY_X / 2, cx(b) + SW_BODY_X / 2, FRONT_WALL, FRONT_WALL + SW_BODY_Y) for b in SW_BX]
    sw_xz = [rect(cx(b) - SW_BODY_X / 2, cx(b) + SW_BODY_X / 2, SW_BOTTOM_Z, LID_UNDER) for b in SW_BX]
    port_xz = [rect(cx(b) - PORT_SHELL_X / 2, cx(b) + PORT_SHELL_X / 2, cz(PORT_BZ) - PORT_SHELL_Z / 2, cz(PORT_BZ) + PORT_SHELL_Z / 2) for b in PORT_BX]
    boss_xy = [rect(x - BOSS_D / 2, x + BOSS_D / 2, y - BOSS_D / 2, y + BOSS_D / 2) for x, y in BOSSES]
    pcb_xy = rect(cx(0), cx(PCB_L + END_PART_X), PCB_FRONT_Y - 0.01, PCB_BACK_Y + BACK_PARTS)
    holes_xy = [rect(cx(b) - SW_HOLE_D / 2, cx(b) + SW_HOLE_D / 2, SW_Y - SW_HOLE_D / 2, SW_Y + SW_HOLE_D / 2) for b in SW_BX]
    for i in range(len(SW_BX)):
        for j, p in enumerate(port_xz):
            if gap(sw_xz[i], p) < 0.2: problems.append(f"switch {i+1} body hits port {j+1} (gap {gap(sw_xz[i], p):.2f} mm)")
        for j, b in enumerate(boss_xy):
            if gap(sw_xy[i], b) < 0.2: problems.append(f"switch {i+1} body hits screw boss {j+1}")
            if gap(holes_xy[i], b) < 0.8: problems.append(f"switch {i+1} hole too close to screw hole {j+1}")
        if sw_xy[i][3] > PCB_FRONT_Y - 0.2: problems.append(f"switch {i+1} body hits the board's front face")
        if sw_xy[i][0] < WALL: problems.append(f"switch {i+1} is outside the case")
        if abs(cx(SW_BX[i]) - cx(LED_BX)) < SW_BODY_X / 2 + LED_D / 2 + 0.5: problems.append(f"switch {i+1} body blocks the LED")
    for j, b in enumerate(boss_xy):
        if gap(b, pcb_xy) < 0.2: problems.append(f"screw boss {j+1} hits the board")
    for i, (bx, bz) in enumerate(HOLES):
        if bz < PCB_W / 2:   # bottom holes get a screw from the front: driver path must miss ports/switches
            for j, p in enumerate(port_xz):
                if gap(rect(cx(bx) - 2, cx(bx) + 2, cz(bz) - 2, cz(bz) + 2), p) < 0: problems.append(f"screw {i+1} is behind port {j+1}")
    if PORT_FACE_Y > FRONT_WALL + 0.01:
        problems.append(f"ports end {PORT_FACE_Y - FRONT_WALL:.1f} mm behind the front wall; plugs may not seat")
    if problems:
        raise SystemExit("Layout doesn't fit:\n  - " + "\n  - ".join(dict.fromkeys(problems)))


def body():
    b = box(0, 0, 0, OUT_L, OUT_D, LID_UNDER) - box(WALL, FRONT_WALL, FLOOR, IN_L, IN_BACK_Y - FRONT_WALL, LID_UNDER)
    sl = IN_BACK_Y - PCB_BACK_Y                       # standoff length, back wall -> board back
    for bx, bz in HOLES:
        b += teardrop_y(cx(bx), PCB_BACK_Y, cz(bz), sl + 0.5, STANDOFF_D)
        if bz < PCB_W / 2:   # bottom row: M2 screw from the front, driven through an access hole
            b -= ycyl(cx(bx), PCB_BACK_Y - 0.1, cz(bz), sl - 0.5, SCREW_PILOT)
            b -= ycyl(cx(bx), -0.1, cz(bz), FRONT_WALL + 0.2, DRIVER_HOLE_D)
        else:                # top row: locating pin into the board's hole
            b += ycyl(cx(bx), PCB_FRONT_Y - 0.5, cz(bz), PCB_T + 0.6, PIN_D)
    for x, y in BOSSES:
        b += cyl(x, y, FLOOR - 0.01, LID_UNDER - FLOOR + 0.01, BOSS_D)
        b -= cyl(x, y, LID_UNDER - 10, 10.1, BOSS_PILOT)
    # front wall: port openings + LED window
    for bx in PORT_BX:
        w, h = PORT_SHELL_X + 2 * PORT_CLEAR, PORT_SHELL_Z + 2 * PORT_CLEAR
        b -= box(cx(bx) - w / 2, -0.1, cz(PORT_BZ) - h / 2, w, FRONT_WALL + 0.2, h)
    b -= ycyl(cx(LED_BX), -0.1, cz(LED_BZ), FRONT_WALL + 0.2, LED_D)
    # cable slot in the end wall, open to the top, in front of the board
    b -= box(OUT_L - WALL - 0.1, PCB_FRONT_Y - CABLE_W - 0.5, cz(CABLE_BZ) - CABLE_W / 2, WALL + 0.2, CABLE_W, LID_UNDER)
    return b


def lid():
    """Modelled outer face down (z=0 is the outer face), which is also how it prints."""
    l = box(0, 0, 0, OUT_L, OUT_D, LID_T)
    # rib behind the board's top edge, so the board can't tip back
    l += box(cx(0), PCB_BACK_Y + 0.2, LID_T - 0.01, PCB_L, RIB_T, RIB_H + 0.01)
    for bx in SW_BX:
        l -= cyl(cx(bx), SW_Y, -0.1, LID_T + 0.2, SW_HOLE_D)
        l -= box(cx(bx) - 0.5, SW_Y - SW_HOLE_D / 2 - 3.5, -0.1, 1.0, 2.5, 0.5)   # ON mark at the front
    for x, y in BOSSES:
        l -= cyl(x, y, -0.1, LID_T + 0.2, LID_SCREW_D)
        l -= cyl(x, y, -0.1, 1.2, CSK_D, LID_SCREW_D)
    return l


def ghosts():
    """Non-printed stand-ins in assembled position, for previews."""
    g = {"pcb": box(cx(0), PCB_FRONT_Y, PCB_Z0, PCB_L, PCB_T, PCB_W)}
    acc = {"ports": None, "switches": None, "levers": None}
    add = lambda k, m: acc.__setitem__(k, m if acc[k] is None else acc[k] + m)
    for bx in PORT_BX:
        add("ports", box(cx(bx) - PORT_SHELL_X / 2, PORT_FACE_Y, cz(PORT_BZ) - PORT_SHELL_Z / 2, PORT_SHELL_X, PORT_H, PORT_SHELL_Z))
    for bx in SW_BX:
        add("switches", box(cx(bx) - SW_BODY_X / 2, FRONT_WALL, SW_BOTTOM_Z, SW_BODY_X, SW_BODY_Y, SW_DEPTH))
        add("levers", cyl(cx(bx), SW_Y, OUT_H, SW_LEVER_H, 2.4, 1.6))
    g.update(acc)
    return g


def to_tri(m):
    mesh = m.to_mesh()
    return trimesh.Trimesh(vertices=np.asarray(mesh.vert_properties)[:, :3], faces=np.asarray(mesh.tri_verts), process=False)


if __name__ == "__main__":
    layout_checks()
    out = Path(__file__).with_name("out"); out.mkdir(exist_ok=True)
    for old in ("usb_hub_tray.stl", "preview_tray.png"):
        (out / old).unlink(missing_ok=True)
    bd, ld = body(), lid()
    for name, m in [("usb_hub_body", bd), ("usb_hub_lid", ld)]:
        tm = to_tri(m)
        assert tm.is_watertight, f"{name} is not watertight"
        tm.export(out / f"{name}.stl")
        print(f"{name}.stl  {tm.extents.round(1)} mm  vol {tm.volume/1000:.1f} cm3")
    prev = out / "_preview_parts"; prev.mkdir(exist_ok=True)
    for name, m in ghosts().items():
        to_tri(m).export(prev / f"{name}.stl")
    to_tri(bd).export(prev / "body.stl")
    to_tri(ld.mirror([0, 0, 1]).translate([0, 0, OUT_H])).export(prev / "lid_assembled.stl")
    print(f"outer {OUT_L:.1f} long x {OUT_D:.1f} deep x {OUT_H:.1f} tall, levers to {OUT_H + SW_LEVER_H:.1f}")
    print(f"port faces {FRONT_WALL - PORT_FACE_Y:.1f} mm into the {FRONT_WALL} mm front wall "
          f"({'flush' if PORT_FACE_Y <= 0.01 else f'{PORT_FACE_Y:.1f} mm behind the outer face'})")
    print(f"gap between port shells {min(PITCH) - PORT_SHELL_X:.2f} mm for a {SW_BODY_X} mm switch body")
