"""Render preview PNGs of the USB hub case with a small numpy z-buffer (no Blender/OpenGL needed).
Run after usb_hub_case.py:   python cad/usb_hub_case/render_preview.py
"""
from pathlib import Path
import numpy as np
import trimesh
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
OUT, PARTS = HERE / "out", HERE / "out" / "_preview_parts"
TRAY, LID = (0.42, 0.47, 0.53), (0.22, 0.45, 0.76)
GHOST = {"pcb": (0.86, 0.84, 0.70), "ports": (0.72, 0.75, 0.80), "switches": (0.18, 0.18, 0.20), "levers": (0.82, 0.82, 0.85)}


def look_at(eye, target, up=(0, 0, 1)):
    eye, target, up = map(np.asarray, (eye, target, up))
    f = target - eye; f = f / np.linalg.norm(f)
    r = np.cross(f, up); r = r / np.linalg.norm(r)
    u = np.cross(r, f)
    return np.stack([r, u, -f]), eye


def render(parts, eye, target, size=(1400, 820), ortho=None, fov=32):
    W, H = size
    R, e = look_at(eye, target)
    zbuf = np.full((H, W), np.inf); img = np.ones((H, W, 3)) * 0.94
    light = R.T @ np.array([0.35, 0.55, 0.75]); light /= np.linalg.norm(light)
    f = (H / 2) / np.tan(np.radians(fov / 2))
    for tm, col in parts:
        v = (tm.vertices - e) @ R.T                  # camera space, looking down -Z
        if ortho: sx = v[:, 0] * (W / ortho) + W / 2; sy = -v[:, 1] * (W / ortho) + H / 2
        else:     sx = f * v[:, 0] / -v[:, 2] + W / 2; sy = -f * v[:, 1] / -v[:, 2] + H / 2
        depth = -v[:, 2]
        n = tm.face_normals
        shade = 0.38 + 0.62 * np.clip(n @ light, 0, 1) ** 0.9
        base = np.array(col)
        for t, s in zip(tm.faces, shade):
            xs, ys, zs = sx[t], sy[t], depth[t]
            x0, x1 = max(int(xs.min()), 0), min(int(xs.max()) + 1, W - 1)
            y0, y1 = max(int(ys.min()), 0), min(int(ys.max()) + 1, H - 1)
            if x0 > x1 or y0 > y1: continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + .5, np.arange(y0, y1 + 1) + .5)
            d = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(d) < 1e-9: continue
            a = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / d
            b = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / d
            c = 1 - a - b
            m = (a >= -1e-4) & (b >= -1e-4) & (c >= -1e-4)
            if not m.any(): continue
            z = a * zs[0] + b * zs[1] + c * zs[2]
            sub = zbuf[y0:y1 + 1, x0:x1 + 1]
            m &= z < sub - 1e-3
            sub[m] = z[m]
            img[y0:y1 + 1, x0:x1 + 1][m] = base * s
    # dark edge lines where depth jumps (cheap outline)
    zf = np.where(np.isinf(zbuf), zbuf[np.isfinite(zbuf)].max() * 2 if np.isfinite(zbuf).any() else 0, zbuf)
    gz = np.maximum(np.abs(np.diff(zf, axis=0, prepend=zf[:1])), np.abs(np.diff(zf, axis=1, prepend=zf[:, :1])))
    img[gz > np.median(zf[np.isfinite(zbuf)]) * 0.02] *= 0.35
    return img


def load(name, offset=0.0, folder=PARTS):
    tm = trimesh.load(folder / name, process=False)
    if offset: tm = tm.copy(); tm.apply_translation([0, 0, offset])
    return tm


if __name__ == "__main__":
    body = load("body.stl")
    ghosts = [(load(f"{n}.stl"), c) for n, c in GHOST.items()]
    lid = lambda lift: (load("lid_assembled.stl", lift), LID)
    shots = [
        # name, lid lift (None = no lid), eye, target, ortho width
        ("preview.png", 35, (-30, -150, 120), (78, 12, 18), None),
        ("preview_assembled.png", 0, (-55, -120, 75), (78, 12, 16), None),
        ("preview_front.png", 0, (78, -400, 21), (78, 0, 21), 172),
        ("preview_top.png", 0, (78, 11.5, 400), (78, 11.55, 0), 172),
        ("preview_open.png", None, (40, -110, 130), (78, 12, 12), None),
    ]
    for fname, lift, eye, tgt, ortho in shots:
        parts = [(body, TRAY)] + [g for g in ghosts if lift is not None or g[1] != GHOST["levers"]]
        if lift is not None: parts.append(lid(lift))
        img = render(parts, eye, tgt, ortho=ortho)
        plt.imsave(OUT / fname, np.clip(img, 0, 1))
        print("wrote", fname)
