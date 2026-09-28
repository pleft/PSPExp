#!/usr/bin/env python3
"""
The PSP's own pictures for the PSPExp EBOOT, in the launcher's look (its
backdrop's navy and glows, the app covers' indigo-to-teal gradient, Inter):
  ICON0.PNG  144x80   the XMB tile
  PIC1.PNG   480x272  the XMB background while PSPExp is highlighted

  scripts/dev/make-eboot-art.py [OUT_DIR] [--variant V] [--all]
      OUT_DIR defaults to psp/boot (the EBOOT's build folder); V defaults to b3n,
      the chosen design; --all writes every variation tried (a, b, c, b1-b8, ...).
Needs Pillow. Drawn 4x larger and scaled down, for smooth edges.
"""
import math, os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT = os.path.join(ROOT, "tools/pspexp-term/assets/Inter-SemiBold.ttf")
FONT_R = os.path.join(ROOT, "tools/pspexp-term/assets/Inter-Regular.ttf")
S = 4   # supersampling

NAVY_TOP, NAVY_BOTTOM = (0x14, 0x17, 0x26), (0x07, 0x08, 0x0e)
INDIGO, TEAL, VIOLET, ACCENT = (0x5b, 0x6c, 0xff), (0x14, 0xb8, 0xc8), (0x9b, 0x5c, 0xff), (0x8b, 0x9b, 0xff)
MUTED = (0x7d, 0x86, 0x9a)


def backdrop(w, h, glows):
    """the launcher's background: a navy gradient and soft coloured glows"""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        base = [NAVY_TOP[i] * (1 - t) + NAVY_BOTTOM[i] * t for i in range(3)]
        for x in range(w):
            c = list(base)
            for (cx, cy, rad, col, k) in glows:
                d = math.hypot(x - cx, y - cy) / rad
                if d < 1:
                    wgt = (1 - d) ** 2 * k
                    for i in range(3):
                        c[i] += col[i] * wgt
            px[x, y] = tuple(min(255, int(v)) for v in c)
    return img


def gradient(w, h, a, b):
    """indigo to teal, top left to bottom right (the app covers')"""
    g = Image.new("RGB", (w, h))
    px = g.load()
    for y in range(h):
        for x in range(w):
            t = min(1, max(0, (x * 0.55 + y * 0.85) / (w * 0.55 + h * 0.85)))
            px[x, y] = tuple(int(a[i] * (1 - t) + b[i] * t) for i in range(3))
    return g


def gradient3(w, h, a, b, c):
    """three colours along the same diagonal"""
    g = Image.new("RGB", (w, h))
    px = g.load()
    for y in range(h):
        for x in range(w):
            t = min(1, max(0, (x * 0.55 + y * 0.85) / (w * 0.55 + h * 0.85)))
            u, p, q = (t * 2, a, b) if t < 0.5 else (t * 2 - 1, b, c)
            px[x, y] = tuple(int(p[i] * (1 - u) + q[i] * u) for i in range(3))
    return g


def gloss(img, r):
    """a soft light on the card's upper half"""
    w, h = img.size
    g = Image.new("L", (w, h), 0)
    ImageDraw.Draw(g).ellipse((-w * 0.2, -h * 0.9, w * 1.2, h * 0.55), fill=60)
    g = g.filter(ImageFilter.GaussianBlur(h * 0.08))
    white = Image.new("RGBA", (w, h), (255, 255, 255, 0))
    white.putalpha(Image.composite(g, Image.new("L", (w, h), 0), rounded_mask(w, h, r)))
    img.alpha_composite(white)


def rounded_mask(w, h, r):
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, w - 1, h - 1), r, fill=255)
    return m


def psp_mark(w, h, body=(255, 255, 255), screen=(0x10, 0x14, 0x22), glow_screen=True, outline=False, tiles=False):
    """a PSP seen from the front, simplified: the body, its wide screen, the
    d-pad and the four face buttons. RGBA, w:h about 2.3:1"""
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    lw = max(2, h // 22)
    sw, sh = int(w * 0.52), int(h * 0.64)
    sx, sy = (w - sw) // 2, (h - sh) // 2
    if outline:   # drawn with a line, the screen a solid block of the same white
        d.rounded_rectangle((lw, lw, w - 1 - lw, h - 1 - lw), h // 2, outline=body + (255,), width=lw * 2)
        d.rounded_rectangle((sx, sy, sx + sw, sy + sh), max(2, h // 14), fill=body + (255,))
    else:
        d.rounded_rectangle((0, 0, w - 1, h - 1), h // 2, fill=body + (255,))
        scr = gradient(sw, sh, (0x1b, 0x21, 0x3a), (0x0c, 0x3a, 0x4a)) if glow_screen else Image.new("RGB", (sw, sh), screen)
        im.paste(scr, (sx, sy), rounded_mask(sw, sh, max(2, h // 14)))
    if tiles and not outline:   # the launcher on the screen: a rail of coloured tiles
        n, g = 4, sw * 0.05
        tw = (sw - g * (n + 1)) / n
        cols = [ACCENT, TEAL, (0xf5, 0xa5, 0x24), (0xf4, 0x72, 0xb6)]
        for k in range(n):
            x0 = sx + g + k * (tw + g)
            d.rounded_rectangle((x0, sy + sh * 0.3, x0 + tw, sy + sh * 0.3 + tw), max(1, h // 30), fill=cols[k] + (255,))
    elif not outline:   # a prompt on the screen: the console
        cx, cy = sx + sw * 0.18, sy + sh * 0.5
        d.line([(cx, cy - sh * 0.16), (cx + sh * 0.16, cy), (cx, cy + sh * 0.16)], fill=ACCENT + (255,), width=lw, joint="curve")
        d.line([(cx + sh * 0.3, cy + sh * 0.17), (cx + sh * 0.62, cy + sh * 0.17)], fill=TEAL + (255,), width=lw)
    # d-pad (left) and face buttons (right), in the body's shadow colour (white when drawn as a line)
    ink = body + (255,) if outline else (0x2a, 0x31, 0x45, 255)
    dx, dy, arm, th = h * 0.5 * 0.72, h * 0.5, h * 0.17, h * 0.075
    d.rounded_rectangle((dx - arm, dy - th, dx + arm, dy + th), th * 0.6, fill=ink)
    d.rounded_rectangle((dx - th, dy - arm, dx + th, dy + arm), th * 0.6, fill=ink)
    bx, r = w - h * 0.5 * 0.72, h * 0.055
    for (ox, oy) in ((0, -1), (1, 0), (0, 1), (-1, 0)):
        x, y = bx + ox * h * 0.13, dy + oy * h * 0.13
        d.ellipse((x - r, y - r, x + r, y + r), fill=ink)
    return im


def shadow(im, radius, alpha=110, offset=(0, 0)):
    a = im.split()[-1].point(lambda v: v * alpha // 255)
    sh = Image.new("RGBA", im.size, (0, 0, 0, 0))
    sh.putalpha(a)
    sh = sh.filter(ImageFilter.GaussianBlur(radius))
    out = Image.new("RGBA", (im.size[0] + abs(offset[0]) * 2, im.size[1] + abs(offset[1]) * 2), (0, 0, 0, 0))
    return sh, out


def wordmark(draw, x, y, size, exp_col=ACCENT, psp_col=(255, 255, 255), anchor="ls"):
    f = ImageFont.truetype(FONT, size)
    w1 = draw.textlength("PSP", font=f)
    w2 = draw.textlength("Exp", font=f)
    if anchor == "ms":   # centred
        x -= (w1 + w2) / 2
    draw.text((x, y), "PSP", font=f, fill=psp_col, anchor="ls")
    draw.text((x + w1, y), "Exp", font=f, fill=exp_col, anchor="ls")
    return w1 + w2


def paste_mark(base, mark, x, y, blur):
    """the mark with a soft shadow under it"""
    a = mark.split()[-1]
    sh = Image.new("RGBA", mark.size, (0, 0, 0, 0))
    sh.putalpha(a.point(lambda v: v * 120 // 255))
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    base.alpha_composite(sh, (int(x), int(y + blur * 0.6)))
    base.alpha_composite(mark, (int(x), int(y)))


def icon0(variant):
    W, H = 144 * S, 80 * S
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    if variant == "a":   # the launcher's dark card, the mark and the name
        card = backdrop(W, H, [(W * 0.2, -H * 0.3, W * 0.9, INDIGO, 0.55), (W * 1.0, H * 1.1, W * 0.8, TEAL, 0.30), (W * 0.6, H * 0.1, W * 0.5, VIOLET, 0.12)]).convert("RGBA")
        img.paste(card, (0, 0), rounded_mask(W, H, 14 * S))
        m = psp_mark(int(W * 0.56), int(W * 0.56 / 2.3))
        paste_mark(img, m, (W - m.size[0]) / 2, H * 0.12, 3 * S)
        wordmark(ImageDraw.Draw(img), W / 2, H * 0.9, 19 * S, anchor="ms")
    elif variant == "b":   # a gradient card, like the built-in apps' covers
        card = gradient(W, H, INDIGO, TEAL).convert("RGBA")
        img.paste(card, (0, 0), rounded_mask(W, H, 14 * S))
        m = psp_mark(int(W * 0.56), int(W * 0.56 / 2.3))
        paste_mark(img, m, (W - m.size[0]) / 2, H * 0.12, 3 * S)
        wordmark(ImageDraw.Draw(img), W / 2, H * 0.9, 19 * S, exp_col=(0x0e, 0x12, 0x20), anchor="ms")
    elif variant.startswith("b"):   # B's family: a gradient card, the mark, the name
        opt = {
            "b1": dict(),                                                            # B as it was
            "b2": dict(exp=(255, 255, 255)),                                         # the name all white
            "b3": dict(g3=(VIOLET, INDIGO, TEAL), exp=(255, 255, 255), shine=True),  # three colours and a soft light
            "b4": dict(tiles=True, exp=(0x0e, 0x12, 0x20)),                          # the launcher on its screen
            "b5": dict(outline=True, exp=(255, 255, 255)),                           # the mark drawn as a line
            "b6": dict(side=True, exp=(0x0e, 0x12, 0x20)),                           # mark left, name right, larger
            "b7": dict(g=(TEAL, INDIGO), exp=(255, 255, 255), shine=True),           # the gradient the other way
            "b8": dict(g3=(INDIGO, VIOLET, (0xf4, 0x72, 0xb6)), exp=(255, 255, 255)),  # warmer: indigo, violet, pink
            "b3n": dict(g3=(VIOLET, INDIGO, TEAL), exp=(0x0e, 0x12, 0x20), shine=True),   # B3, "Exp" navy
            "b3d": dict(g3=(VIOLET, INDIGO, TEAL), exp=(0x1e, 0x1b, 0x4b), shine=True),   # B3, "Exp" deep indigo
            "b8n": dict(g3=(INDIGO, VIOLET, (0xf4, 0x72, 0xb6)), exp=(0x0e, 0x12, 0x20)),  # B8, "Exp" navy
            "b8d": dict(g3=(INDIGO, VIOLET, (0xf4, 0x72, 0xb6)), exp=(0x3b, 0x07, 0x64)),  # B8, "Exp" deep plum
        }[variant]
        a_, b_ = opt.get("g", (INDIGO, TEAL))
        card = (gradient3(W, H, *opt["g3"]) if "g3" in opt else gradient(W, H, a_, b_)).convert("RGBA")
        img.paste(card, (0, 0), rounded_mask(W, H, 14 * S))
        if opt.get("shine"): gloss(img, 14 * S)
        d = ImageDraw.Draw(img)
        if opt.get("side"):
            m = psp_mark(int(W * 0.42), int(W * 0.42 / 2.3), tiles=opt.get("tiles", False))
            paste_mark(img, m, W * 0.05, (H - m.size[1]) / 2, 3 * S)
            f = ImageFont.truetype(FONT, 20 * S)
            d.text((W * 0.52, H * 0.53), "PSP", font=f, fill=(255, 255, 255), anchor="ls")
            d.text((W * 0.52, H * 0.53 + 19 * S), "Exp", font=f, fill=opt.get("exp", (0x0e, 0x12, 0x20)), anchor="ls")
        else:
            m = psp_mark(int(W * 0.56), int(W * 0.56 / 2.3), tiles=opt.get("tiles", False), outline=opt.get("outline", False))
            paste_mark(img, m, (W - m.size[0]) / 2, H * 0.12, 3 * S)
            wordmark(d, W / 2, H * 0.9, 19 * S, exp_col=opt.get("exp", (0x0e, 0x12, 0x20)), anchor="ms")
    else:   # no card: the mark on a gradient tile, the name beside it (the XMB shows through)
        t = 62 * S
        tile = gradient(t, t, INDIGO, TEAL).convert("RGBA")
        tm = rounded_mask(t, t, 16 * S)
        ts = Image.new("RGBA", (t, t), (0, 0, 0, 0)); ts.putalpha(tm.point(lambda v: v * 110 // 255))
        img.alpha_composite(ts.filter(ImageFilter.GaussianBlur(3 * S)), (4 * S, 11 * S))
        img.paste(tile, (4 * S, 9 * S), tm)
        m = psp_mark(int(t * 0.82), int(t * 0.82 / 2.3))
        img.alpha_composite(m, (4 * S + (t - m.size[0]) // 2, 9 * S + (t - m.size[1]) // 2))
        d = ImageDraw.Draw(img)
        wordmark(d, 71 * S, 44 * S, 17 * S)
        d.text((72 * S, 57 * S), "console", font=ImageFont.truetype(FONT_R, 9 * S), fill=(0xc9, 0xd1, 0xe4), anchor="ls")
    return img.resize((144, 80), Image.LANCZOS)


def pic1():
    """the XMB background: the launcher's backdrop, the mark large and faint on the right"""
    W, H = 480, 272
    img = backdrop(W, H, [(90, -60, 330, INDIGO, 0.30), (500, 300, 300, TEAL, 0.13), (300, 40, 220, VIOLET, 0.08)]).convert("RGBA")
    bw = 150   # small and faint, low in the corner: the XMB's menu text is over the middle
    big = psp_mark(bw * S, int(bw * S / 2.3)).resize((bw, int(bw / 2.3)), Image.LANCZOS)
    big.putalpha(big.split()[-1].point(lambda v: v * 14 // 255))
    img.alpha_composite(big, (W - bw - 16, H - int(bw / 2.3) - 14))
    return img.convert("RGB")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = args[0] if args else os.path.join(ROOT, "psp/boot")
    variant = "b3n"   # the chosen one (2026-09-28): violet-indigo-teal with a soft light, "Exp" in navy
    if "--variant" in sys.argv:
        variant = sys.argv[sys.argv.index("--variant") + 1]
    os.makedirs(out, exist_ok=True)
    if "--all" in sys.argv:
        for v in ["a", "b", "c"] + ["b%d" % i for i in range(1, 9)] + ["b3n", "b3d", "b8n", "b8d"]:
            icon0(v).save(os.path.join(out, "ICON0-%s.PNG" % v))
    else:
        icon0(variant).save(os.path.join(out, "ICON0.PNG"))
    pic1().save(os.path.join(out, "PIC1.PNG"))
    print("wrote", out)


if __name__ == "__main__":
    main()
