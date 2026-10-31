/*
 * PSPExp's themes (2026-10-31): its colours are blues, and a theme turns them into another hue, or
 * (saturation scaled down) into greys. Only a blue, a blue-grey or an indigo turns (hue 0.48 to
 * 0.78, saturation over 0.12), so white and grey text, covers, photos, the favourites' yellow, a
 * warning's red, the Ready green and the PSP's button colours stay as they are.
 *
 * Shared by the console's launcher (every colour it draws, tools/pspexp-term/launcher_gfx.c) and
 * the PSP (its own pictures: the hint bar, the volume bar, the card when the board goes; THEME),
 * so both turn a colour the same way. No libm: the PSP runtime links none.
 */
#ifndef PSPEXP_THEME_H
#define PSPEXP_THEME_H

static float pspexp_theme_hue2rgb(float p, float q, float t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

/* r, g, b (0..1) turned in place: hue added (a turn is 1.0), saturation multiplied by sat */
static void pspexp_theme_px(float *r, float *g, float *b, float hue, float sat)
{
    float mx, mn, h, s, l, d;
    if (hue == 0.0f && sat == 1.0f) return;
    mx = *r > *g ? *r : *g; if (*b > mx) mx = *b;
    mn = *r < *g ? *r : *g; if (*b < mn) mn = *b;
    l = (mx + mn) / 2; d = mx - mn;
    if (d < 1e-6f) return;
    s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == *r) h = (*g - *b) / d + (*g < *b ? 6 : 0);
    else if (mx == *g) h = (*b - *r) / d + 2;
    else h = (*r - *g) / d + 4;
    h /= 6;
    if (h <= 0.48f || h >= 0.78f || s <= 0.12f) return;   /* (not one of PSPExp's blues) */
    h += hue;
    while (h < 0) h += 1;
    while (h >= 1) h -= 1;
    s *= sat;
    {
        float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        *r = pspexp_theme_hue2rgb(p, q, h + 1.0f / 3); *g = pspexp_theme_hue2rgb(p, q, h); *b = pspexp_theme_hue2rgb(p, q, h - 1.0f / 3);
    }
}

#endif
