#!/usr/bin/env python3
"""PSPExp cartridge: a Raspberry Pi Zero 2 W, its battery and power module in a
shell that sits on the PSP-1000's top edge between L and R, like a Game Boy
cartridge. A saddle grips the PSP's front and back faces, and a Mini-B plug in
the saddle's roof goes into the PSP's USB port. The plug only carries the
signals; the saddle takes the load, so the port is never a lever.

Parametric model (manifold3d). Writes STL files to out/:
  cart_front.stl, cart_back.stl   the two printed halves (split at the PSP's
                                  mid-plane, M2 screws into heat-set inserts)
  psp_*.stl, part_*.stl           the PSP-1000 reference and the parts inside,
                                  for the viewer and for checking fit
  assembly.json                   part list with colours and parameters
  model.json                      the same with every STL inside (base64), for
                                  viewer.html

  build/cad-venv/bin/python hardware/cartridge/cartridge.py

Coordinates (mm): X along the PSP's width (0 = left end, looking at the
screen), Y up (0 = the PSP's bottom edge, PSP_H = its top edge), Z through the
PSP (0 = mid-plane, + = screen side). Values marked MEASURE are estimates to be
replaced by calipers on a real PSP-1000.
"""
import base64
import json
import os
import struct
import sys

import numpy as np
from manifold3d import Manifold, set_circular_segments

set_circular_segments(48)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")

# ---- the PSP-1000 --------------------------------------------------------
PSP_W, PSP_H, PSP_T = 170.0, 74.0, 23.0  # body (Sony: 170 x 74 x 23)
PSP_R = 18.0          # corner radius seen from the front (MEASURE)
PSP_EDGE = 4.0        # rounding of the front/back edges (MEASURE)
USB_X = 85.0          # centre of the Mini-B port along the top edge (MEASURE)
USB_Z = 0.0           # its centre through the thickness, + toward the screen (MEASURE)
SHOULDER_IN = 40.0    # L ends / R starts this far from each end (MEASURE)
BEZEL_TOP = 9.0       # front face above the screen's glass (MEASURE)

# ---- the cartridge -------------------------------------------------------
FIT = 0.3             # gap between the saddle and the PSP per side
WALL = 2.0            # shell walls
ROOF = 2.5            # saddle roof (sits on the PSP's top edge)
CART_W = 84.0         # width along the top edge (clears L and R)
CART_H = 42.0         # height above the PSP's top edge
CART_R = 5.0          # corner radius of the top corners
LIP_FRONT = 6.0       # saddle lip over the screen side (< BEZEL_TOP)
LIP_BACK = 12.0       # saddle lip over the back (UMD door side)
SNAP = 0.5            # inward bump near each lip's bottom edge
SPLIT_Z = 0.0         # front / back halves meet here
CART_X = USB_X        # cartridge centred on the port
CART_T = PSP_T + 2 * FIT + 2 * WALL
Y0 = PSP_H            # the PSP's top edge

# ---- what goes inside ----------------------------------------------------
PI = dict(w=65.0, h=30.0, pcb=1.2, holes=(58.0, 23.0), hole_d=2.75, standoff=3.0)
BATT = dict(w=50.0, h=34.0, t=5.0)           # LiPo 503450, 1000 mAh
POWER = dict(w=23.0, h=36.0, t=5.0)          # USB-C charger + 5 V boost module
PLUG = dict(shell_w=6.9, shell_t=3.1, shell_l=5.4, body_w=9.0, body_t=5.0, body_h=10.0, float=0.4)
SWITCH = dict(w=8.6, t=3.8)                  # slide switch slot on the top face
USBC = dict(w=9.4, t=3.6)                    # charging port on the top face
M2_INSERT_D, M2_CLEAR_D, M2_HEAD_D = 3.2, 2.3, 4.2


def box(x0, y0, z0, x1, y1, z1):
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])


def cyl_z(x, y, z0, z1, r):
    return Manifold.cylinder(z1 - z0, r).translate([x, y, z0])


def cyl_y(x, z, y0, y1, r):
    # a cylinder along Y: build along Z, then turn -90 about X (Z -> Y)
    return Manifold.cylinder(y1 - y0, r).rotate([-90, 0, 0]).translate([x, y0, z])


def cyl_x(y, z, x0, x1, r):
    return Manifold.cylinder(x1 - x0, r).rotate([0, 90, 0]).translate([x0, y, z])


def rounded_slab(x0, y0, x1, y1, t, r, e):
    """Front-view rounded rectangle, thickness t centred on Z=0, with its
    front and back edges rounded by e."""
    parts = []
    for cx in (x0 + r, x1 - r):
        for cy in (y0 + r, y1 - r):
            parts.append(cyl_z(cx, cy, -t / 2 + e, t / 2 - e, r))
            parts.append(cyl_z(cx, cy, -t / 2, t / 2, r - e))
    return Manifold.batch_hull(parts)


# ---- PSP-1000 reference ----------------------------------------------------
def psp():
    body = rounded_slab(0, 0, PSP_W, PSP_H, PSP_T, PSP_R, PSP_EDGE)
    port = box(USB_X - 3.6, Y0 - 6, USB_Z - 1.7, USB_X + 3.6, Y0 + 1, USB_Z + 1.7)
    body = body - port
    screen_w, screen_h = 97.0, 56.0
    screen = box(PSP_W / 2 - screen_w / 2, PSP_H / 2 - screen_h / 2, PSP_T / 2 - 0.2,
                 PSP_W / 2 + screen_w / 2, PSP_H / 2 + screen_h / 2, PSP_T / 2 + 0.3)
    shoulders = Manifold.batch_boolean([
        rounded_slab(3, Y0 - 5, SHOULDER_IN, Y0 + 0.8, 14, 4, 2).translate([0, 0, -2]),
        rounded_slab(PSP_W - SHOULDER_IN, Y0 - 5, PSP_W - 3, Y0 + 0.8, 14, 4, 2).translate([0, 0, -2]),
    ], __import__("manifold3d").OpType.Add)
    controls = [cyl_z(22, 38, PSP_T / 2 - 1, PSP_T / 2 + 1.5, 3.2 * 1.0)]
    for dx, dy in ((0, 8), (0, -8), (8, 0), (-8, 0)):       # d-pad arms
        controls.append(box(22 + dx - 3.5, 38 + dy - 3.5, PSP_T / 2 - 1, 22 + dx + 3.5, 38 + dy + 3.5, PSP_T / 2 + 1.2))
    for dx, dy in ((0, 9), (0, -9), (9, 0), (-9, 0)):       # face buttons
        controls.append(cyl_z(148 + dx, 38 + dy, PSP_T / 2 - 1, PSP_T / 2 + 1.5, 3.6))
    controls.append(cyl_z(20, 17, PSP_T / 2 - 1, PSP_T / 2 + 1.8, 5.5))   # nub
    return dict(psp_body=body, psp_screen=screen, psp_shoulders=shoulders,
                psp_controls=Manifold.batch_boolean(controls, __import__("manifold3d").OpType.Add))


# ---- the cartridge shell --------------------------------------------------
def shell():
    x0, x1 = CART_X - CART_W / 2, CART_X + CART_W / 2
    zt = CART_T / 2
    # outer: rounded top corners, square bottom (meets the PSP's straight edge)
    top = rounded_slab(x0, Y0 - LIP_BACK, x1, Y0 + CART_H, CART_T, CART_R, 1.5)
    bottom = box(x0, Y0 - LIP_BACK, -zt, x1, Y0 + CART_H - CART_R, zt)
    outer = top + (bottom ^ rounded_slab(x0 - 10, Y0 - LIP_BACK, x1 + 10, Y0 + CART_H, CART_T, 0.01 + CART_R, 1.5))
    # the saddle channel the PSP's top edge fits into
    ch_z0, ch_z1 = -PSP_T / 2 - FIT, PSP_T / 2 + FIT
    channel = box(x0 - 1, Y0 - LIP_BACK - 1, ch_z0, x1 + 1, Y0 + FIT, ch_z1)
    # the front lip is shorter: it must stay above the screen
    front_cut = box(x0 - 1, Y0 - LIP_BACK - 1, ch_z1 - 0.01, x1 + 1, Y0 - LIP_FRONT, zt + 1)
    s = outer - channel - front_cut
    # snap bumps along each lip's inner face
    s = s + (cyl_x(Y0 - LIP_FRONT + 1.6, ch_z1 + 0.8 - SNAP, x0 + 4, x1 - 4, 0.8)
             ^ box(x0, Y0 - LIP_FRONT, ch_z1 - SNAP, x1, Y0, ch_z1))
    s = s + (cyl_x(Y0 - LIP_BACK + 2.0, ch_z0 - 0.8 + SNAP, x0 + 4, x1 - 4, 0.8)
             ^ box(x0, Y0 - LIP_BACK, ch_z0, x1, Y0, ch_z0 + SNAP))
    # the hollow above the roof
    cav = box(x0 + WALL, Y0 + FIT + ROOF, -zt + WALL, x1 - WALL, Y0 + CART_H - WALL, zt - WALL)
    s = s - cav
    # the plug: its metal shell passes through the roof, its body sits in a
    # pocket above it with a little float so the port is never forced
    p = PLUG
    shell_slot = box(USB_X - p["shell_w"] / 2 - 0.25, Y0 - 1, USB_Z - p["shell_t"] / 2 - 0.25,
                     USB_X + p["shell_w"] / 2 + 0.25, Y0 + FIT + ROOF + 1, USB_Z + p["shell_t"] / 2 + 0.25)
    pw, pt, ph, f = p["body_w"] / 2 + p["float"], p["body_t"] / 2 + p["float"], p["body_h"], p["float"]
    ry = Y0 + FIT + ROOF
    cradle = (box(USB_X - pw - 1.6, ry, USB_Z - pt - 1.6, USB_X + pw + 1.6, ry + ph + 1.6, USB_Z + pt + 1.6)
              - box(USB_X - pw, ry - 0.1, USB_Z - pt, USB_X + pw, ry + ph + f, USB_Z + pt)
              - box(USB_X - 2.5, ry + ph - 1, USB_Z - pt, USB_X + 2.5, ry + ph + 3, USB_Z + pt))  # wires out
    s = s + cradle - shell_slot
    # Pi Zero standoffs from the front wall (components face the back)
    pz_front = zt - WALL
    for sx in (-1, 1):
        for sy in (-1, 1):
            hx = CART_X + sx * PI["holes"][0] / 2
            hy = pi_y0() + PI["h"] / 2 + sy * PI["holes"][1] / 2
            s = s + cyl_z(hx, hy, pz_front - PI["standoff"], pz_front + 0.1, 2.6)
            s = s - cyl_z(hx, hy, pz_front - PI["standoff"] - 1, pz_front - 0.6, 1.1)  # M2.5 self-tap
    # screw bosses: inserts in the front half, clearance + head in the back
    for bx, by in screw_points():
        s = s + cyl_z(bx, by, -zt + WALL - 0.1, zt - WALL + 0.1, 3.0)
        s = s - cyl_z(bx, by, SPLIT_Z, SPLIT_Z + 5.0, M2_INSERT_D / 2)
        s = s - cyl_z(bx, by, -zt - 1, SPLIT_Z, M2_CLEAR_D / 2)
        s = s - cyl_z(bx, by, -zt - 1, -zt + 2.0, M2_HEAD_D / 2)
    # top face: charging port over the power module, power switch, LED
    yt0, yt1 = Y0 + CART_H - WALL - 1, Y0 + CART_H + 1
    px, pz = power_pos()
    s = s - box(px - USBC["w"] / 2, yt0, pz - USBC["t"] / 2, px + USBC["w"] / 2, yt1, pz + USBC["t"] / 2)
    s = s - box(CART_X - 28 - SWITCH["w"] / 2, yt0, -SWITCH["t"] / 2, CART_X - 28 + SWITCH["w"] / 2, yt1, SWITCH["t"] / 2)
    s = s - cyl_y(CART_X - 14, 0, yt0, yt1, 1.1)
    # vents over the Pi's processor (middle layer)
    for i in range(5):
        vx = CART_X - 12 + i * 4.0
        s = s - box(vx - 0.9, yt0, -5, vx + 0.9, yt1, 5)
    # SD card slot on the left end, for flashing without opening the shell
    sd_y = pi_y0() + PI["h"] / 2
    s = s - box(x0 - 1, sd_y - 7.5, pi_z_back() - 1.8, x0 + WALL + 1, sd_y + 7.5, pi_z_back() + 0.4)
    # label recess on the front, like a cartridge label
    s = s - box(CART_X - 30, Y0 + 7, zt - 0.4, CART_X + 30, Y0 + CART_H - 5, zt + 1)
    # grip ridges on both ends
    for i in range(4):
        gy = Y0 + 12 + i * 5.0
        for gx in (x0, x1):
            s = s - box(gx - 0.6, gy, -zt + 3, gx + 0.6, gy + 1.6, zt - 3)
    front = s ^ box(-1e3, -1e3, SPLIT_Z, 1e3, 1e3, 1e3)
    back = s ^ box(-1e3, -1e3, -1e3, 1e3, 1e3, SPLIT_Z)
    return dict(cart_front=front, cart_back=back)


def pi_y0():
    return Y0 + FIT + ROOF + PLUG["body_h"] + 3.5 - 8.0  # board bottom edge, above the plug cradle's shoulders


def pi_z_back():
    return CART_T / 2 - WALL - PI["standoff"] - PI["pcb"]


def power_pos():
    return CART_X + 18.0, 0.0     # module centre (x, z): middle layer, right of the vents


def screw_points():
    x0, x1 = CART_X - CART_W / 2 + WALL + 3.2, CART_X + CART_W / 2 - WALL - 3.2
    return [(x0, Y0 + FIT + ROOF + 4.0), (x1, Y0 + FIT + ROOF + 4.0),
            (x0, Y0 + CART_H - WALL - 4.0), (x1, Y0 + CART_H - WALL - 4.0)]


# ---- the parts inside (for fit and the viewer) ----------------------------
def parts():
    out = {}
    zb = pi_z_back()
    y0 = pi_y0()
    pcb = box(CART_X - PI["w"] / 2, y0, zb, CART_X + PI["w"] / 2, y0 + PI["h"], zb + PI["pcb"])
    for sx in (-1, 1):
        for sy in (-1, 1):
            pcb = pcb - cyl_z(CART_X + sx * PI["holes"][0] / 2, y0 + PI["h"] / 2 + sy * PI["holes"][1] / 2,
                              zb - 1, zb + 2, PI["hole_d"] / 2)
    soc = box(CART_X - 7, y0 + 8, zb - 1.3, CART_X + 7, y0 + 22, zb)
    sd = box(CART_X - PI["w"] / 2 - 1.5, y0 + PI["h"] / 2 - 6, zb - 1.4, CART_X - PI["w"] / 2 + 10, y0 + PI["h"] / 2 + 6, zb)
    ports = box(CART_X - 22, y0 - 1, zb - 3.0, CART_X + 25, y0 + 6, zb)   # mini-HDMI + micro-USB (unused)
    out["part_pizero"] = pcb + soc + sd + ports
    zt = CART_T / 2
    by0 = Y0 + FIT + ROOF + 1.0
    out["part_battery"] = box(CART_X - BATT["w"] / 2 - 6, by0, -zt + WALL + 0.5,
                              CART_X + BATT["w"] / 2 - 6, by0 + BATT["h"], -zt + WALL + 0.5 + BATT["t"])
    px, pz = power_pos()
    ytop = Y0 + CART_H - WALL - 0.3
    out["part_power"] = box(px - POWER["w"] / 2, ytop - POWER["h"], pz - POWER["t"] / 2,
                            px + POWER["w"] / 2, ytop, pz + POWER["t"] / 2)
    p = PLUG
    ry = Y0 + FIT + ROOF
    out["part_plug"] = (box(USB_X - p["shell_w"] / 2, Y0 - p["shell_l"] + FIT + ROOF, USB_Z - p["shell_t"] / 2,
                            USB_X + p["shell_w"] / 2, ry, USB_Z + p["shell_t"] / 2)
                        + box(USB_X - p["body_w"] / 2, ry, USB_Z - p["body_t"] / 2,
                              USB_X + p["body_w"] / 2, ry + p["body_h"], USB_Z + p["body_t"] / 2))
    out["part_switch"] = box(CART_X - 28 - 4.3, ytop - 6, -1.8, CART_X - 28 + 4.3, ytop + 1.8, 1.8)
    return out


# ---- output ----------------------------------------------------------------
def write_stl(path, m):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3].astype(np.float32)
    t = np.asarray(mesh.tri_verts)
    tri = v[t]
    n = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
    rec = np.zeros(len(t), dtype=[("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")])
    rec["n"], rec["v"] = n, tri
    with open(path, "wb") as f:
        f.write(b"PSPExp cartridge".ljust(80, b" "))
        f.write(struct.pack("<I", len(t)))
        f.write(rec.tobytes())


COLOURS = dict(psp_body="#2b2d31", psp_screen="#0b0c0e", psp_shoulders="#3a3d42", psp_controls="#55585e",
               cart_front="#c9cdd4", cart_back="#aeb3bb", part_pizero="#1f7a3a", part_battery="#3c6fc4",
               part_power="#8a3fb8", part_plug="#b8b8b8", part_switch="#e0a030")


def main():
    os.makedirs(OUT, exist_ok=True)
    everything = {**psp(), **shell(), **parts()}
    info = []
    for name, m in everything.items():
        if m.is_empty():
            sys.exit(f"{name}: empty (check the parameters)")
        write_stl(os.path.join(OUT, name + ".stl"), m)
        bb = m.bounding_box()
        info.append(dict(name=name, file=name + ".stl", colour=COLOURS[name], volume_cm3=round(m.volume() / 1000, 2),
                         bbox=[round(x, 2) for x in bb]))
        print(f"{name:14s} {m.num_tri():6d} tris  {m.volume() / 1000:7.2f} cm3  bbox {[round(x, 1) for x in bb]}")
    # interference checks: the parts must not touch the shell or each other
    sh = everything["cart_front"] + everything["cart_back"]
    inside = [k for k in everything if k.startswith("part_") and k != "part_plug"]
    for k in inside:
        v = (everything[k] ^ sh).volume()
        print(f"  {k} overlaps the shell: {v:.2f} mm3")
    for i, a in enumerate(inside):
        for b in inside[i + 1:]:
            v = (everything[a] ^ everything[b]).volume()
            if v > 0.01:
                print(f"  {a} overlaps {b}: {v:.2f} mm3")
    v = (sh ^ everything["psp_body"]).volume()
    print(f"  shell overlaps the PSP body: {v:.2f} mm3 (snap bumps only)")
    v = (sh ^ everything["psp_shoulders"]).volume()
    print(f"  shell overlaps L/R: {v:.2f} mm3")
    params = dict(CART_W=CART_W, CART_H=CART_H, CART_T=CART_T, LIP_FRONT=LIP_FRONT,
                  LIP_BACK=LIP_BACK, USB_X=USB_X, USB_Z=USB_Z, PSP=[PSP_W, PSP_H, PSP_T])
    with open(os.path.join(OUT, "assembly.json"), "w") as f:
        json.dump(dict(parts=info, params=params), f, indent=1)
    for p in info:
        with open(os.path.join(OUT, p["file"]), "rb") as f:
            p["stl"] = base64.b64encode(f.read()).decode()
    with open(os.path.join(OUT, "model.json"), "w") as f:
        json.dump(dict(parts=info, params=params), f)


if __name__ == "__main__":
    main()
