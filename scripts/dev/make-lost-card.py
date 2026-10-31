#!/usr/bin/env python3
# The card the PSP shows on its last picture when the board went (the link lost, or the board
# said goodbye): "Connection lost", waiting for it, HOME to quit and triangle for the
# status page, in the launcher's look (Inter, its Lucide icons, its button badges), written as an
# 8888 texture for the GE executor:
#   python3 scripts/dev/make-lost-card.py   ->  psp/executor-ge/lost_card.h
# (PIL; the fonts are the launcher's, tools/pspexp-term/assets). Run again after changing the text
# or the style, and commit the header with it.
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
A = os.path.join(ROOT, "tools", "pspexp-term", "assets")
OUT = os.path.join(ROOT, "psp", "executor-ge", "lost_card.h")
S = 4   # drawn at 4x, then reduced: smooth edges


def font(name, px):
    """Inter at the launcher's stb_truetype pixel height (ascent minus descent), not PIL's em size"""
    f = ImageFont.truetype(os.path.join(A, name), 100)
    a, d = f.getmetrics()
    return ImageFont.truetype(os.path.join(A, name), round(100 * px / (a + d) * S))


title_f, text_f = font("Inter-SemiBold.ttf", 15.0), font("Inter-Regular.ttf", 11.0)
small_f, badge_f = font("Inter-Regular.ttf", 10.0), font("Inter-SemiBold.ttf", 7.5)
icon_f = ImageFont.truetype(os.path.join(A, "lucide.ttf"), 26 * S)
TITLE, TEXT = "Connection lost", "Waiting to reconnect: check USB cable."
HOME, QUIT, DETAILS = "HOME", "Quit", "Status page"
PANEL, LINE, BADGE_BG = (16, 26, 48, 242), (40, 58, 92, 255), (0x1E, 0x2C, 0x4A, 255)
SUB, WHITE, AMBER, GREEN = (0xA9, 0xB9, 0xD3, 255), (0xF1, 0xF4, 0xF9, 255), (0xFF, 0xB5, 0x47, 255), (0x5E, 0xE3, 0xA1, 255)

W, H = 300, 100
TW = 512   # the texture's sides: powers of two; its rows STRIDE pixels apart (only the card kept)
TH = 128
STRIDE = (W + 7) // 8 * 8
im = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.rounded_rectangle((0, 0, W * S - 1, H * S - 1), radius=12 * S, fill=PANEL)
d.rounded_rectangle((0, 0, W * S - 1, H * S - 1), radius=12 * S, outline=LINE, width=S)
# the icon (Lucide "unplug") in amber, the title and the line beside it
d.text((24 * S, 33 * S), chr(0xE45D), font=icon_f, fill=AMBER, anchor="mm")
d.text((46 * S, 24 * S), TITLE, font=title_f, fill=WHITE, anchor="lm")
d.text((46 * S, 43 * S), TEXT, font=text_f, fill=SUB, anchor="lm")
# the hints under their buttons: HOME on the left, triangle on the right
d.line([(14 * S, 64 * S), ((W - 14) * S, 64 * S)], fill=LINE, width=S)
hy = 82
bw = int(badge_f.getlength(HOME) / S) + 10
d.rounded_rectangle((14 * S, (hy - 7) * S, (14 + bw) * S, (hy + 7) * S), radius=6 * S, fill=BADGE_BG)
d.text(((14 + bw / 2) * S, hy * S), HOME, font=badge_f, fill=SUB, anchor="mm")
d.text(((14 + bw + 5) * S, hy * S), QUIT, font=small_f, fill=SUB, anchor="lm")
lw = small_f.getlength(DETAILS) / S
tx = W - 14 - lw
cx, cy = tx - 5 - 7, hy
d.ellipse(((cx - 7) * S, (cy - 7) * S, (cx + 7) * S, (cy + 7) * S), fill=(0x22, 0x28, 0x36, 255))
d.polygon([(cx * S, (cy - 3.2) * S), ((cx + 3) * S, (cy + 2.2) * S), ((cx - 3) * S, (cy + 2.2) * S)], outline=GREEN, width=int(1.4 * S))
d.text((tx * S, hy * S), DETAILS, font=small_f, fill=SUB, anchor="lm")
im = im.resize((W, H), Image.LANCZOS)

px = []
for y in range(H):
    for x in range(STRIDE):
        r, g, b, al = im.getpixel((x, y)) if x < W else (0, 0, 0, 0)
        px.append(al << 24 | b << 16 | g << 8 | r)   # GU_PSM_8888: ABGR (8 bits a colour: a theme turns them on the PSP, 4 lost its dark blues' hue)
with open(OUT, "w") as f:
    f.write("/* made by scripts/dev/make-lost-card.py: the PSP's \"Connection lost\" card, GU_PSM_8888,\n"
            " * LOST_CARD_H rows of LOST_CARD_STRIDE pixels (a LOST_CARD_TW x LOST_CARD_TH texture, the rest never read) */\n")
    f.write("#define LOST_CARD_TW %d\n#define LOST_CARD_TH %d\n#define LOST_CARD_STRIDE %d\n#define LOST_CARD_W %d\n#define LOST_CARD_H %d\n"
            % (TW, TH, STRIDE, W, H))
    f.write("static const uint32_t g_lost_card[LOST_CARD_STRIDE * LOST_CARD_H] __attribute__((aligned(16))) = {\n")
    for i in range(0, len(px), 8):
        f.write("    " + ", ".join("0x%08x" % v for v in px[i:i + 8]) + ",\n")
    f.write("};\n")
im.save(os.path.join(os.environ.get("HINT_PREVIEW_DIR", "/tmp"), "lost-card.png"))
print("%s: %dx%d (rows of %d) in a %dx%d texture" % (OUT, W, H, STRIDE, TW, TH))
