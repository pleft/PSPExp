#!/usr/bin/env python3
# The bar the PSP shows after a first HOME in an app or a game ("HOME again to quit"), drawn in the
# launcher's look (Inter, its badge pill) and written as an 8888 texture for the GE executor:
#   python3 scripts/dev/make-home-hint.py   ->  psp/executor-ge/home_hint.h
# (PIL; the fonts are the launcher's, tools/pspexp-term/assets). Run again after changing the text
# or the style, and commit the header with it.
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
A = os.path.join(ROOT, "tools", "pspexp-term", "assets")
OUT = os.path.join(ROOT, "psp", "executor-ge", "home_hint.h")


def font(name, px):
    """Inter at the launcher's stb_truetype pixel height (ascent minus descent), not PIL's em size"""
    f = ImageFont.truetype(os.path.join(A, name), 100)
    a, d = f.getmetrics()
    return ImageFont.truetype(os.path.join(A, name), round(100 * px / (a + d)))


badge_f, text_f = font("Inter-SemiBold.ttf", 10.0), font("Inter-Regular.ttf", 13.0)
BADGE, TEXT = "HOME", "again to quit"
PANEL, BADGE_BG, SUB, WHITE = (16, 26, 48, 235), (0x1E, 0x2C, 0x4A, 255), (0xA9, 0xB9, 0xD3, 255), (0xF1, 0xF4, 0xF9, 255)

bw = int(badge_f.getlength(BADGE)) + 10
tw = int(text_f.getlength(TEXT))
W, H = 12 + bw + 8 + tw + 14, 28
TW, TH = 256, 32   # the texture: power-of-two sides
assert W <= TW
im = Image.new("RGBA", (TW, TH), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.rounded_rectangle((0, 0, W - 1, H - 1), radius=8, fill=PANEL)
bx, by = 12, (H - 14) // 2
d.rounded_rectangle((bx, by, bx + bw - 1, by + 13), radius=6, fill=BADGE_BG)
a, dsc = badge_f.getmetrics()
d.text((bx + 5, by + (14 - (a + dsc)) / 2), BADGE, font=badge_f, fill=SUB)
a, dsc = text_f.getmetrics()
d.text((bx + bw + 8, (H - (a + dsc)) / 2), TEXT, font=text_f, fill=WHITE)

px = []
for y in range(TH):
    for x in range(TW):
        r, g, b, al = im.getpixel((x, y))
        px.append(al << 24 | b << 16 | g << 8 | r)   # GU_PSM_8888: ABGR (8 bits a colour: a theme turns them on the PSP, 4 lost its dark blues' hue)
with open(OUT, "w") as f:
    f.write("/* made by scripts/dev/make-home-hint.py: the PSP's \"HOME again to quit\" bar, GU_PSM_8888 */\n")
    f.write("#define HOME_HINT_TW %d\n#define HOME_HINT_TH %d\n#define HOME_HINT_W %d\n#define HOME_HINT_H %d\n" % (TW, TH, W, H))
    f.write("static const uint32_t g_home_hint[HOME_HINT_TW * HOME_HINT_TH] __attribute__((aligned(16))) = {\n")
    for i in range(0, len(px), 8):
        f.write("    " + ", ".join("0x%08x" % v for v in px[i:i + 8]) + ",\n")
    f.write("};\n")
im.crop((0, 0, W, H)).save(os.path.join(os.environ.get("HINT_PREVIEW_DIR", "/tmp"), "home-hint.png"))
print("%s: %dx%d in a %dx%d texture" % (OUT, W, H, TW, TH))
