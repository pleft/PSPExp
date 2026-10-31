/* The launcher's theme on the PSP's own pictures (THEME, protocol/pspexp_theme.h): the colour turn the
 * launcher and the PSP share, and the message, through the null executor's runtime as test_home: sent
 * when set, again after a reconnect (a PSP started again has the default), not to a PSP without it.
 * Added 2026-10-31: the card when the board goes and the "HOME again to quit" bar stayed blue in
 * every theme, the PSP never told */
#include "test_util.h"
#include "../host/libpspexp/pspexp.h"
#include "../host/executor-null/pspexp_null.h"
#include "../runtime/pspexp_runtime.h"
#include "../protocol/pspexp_theme.h"
#include <math.h>
#include <string.h>

static int g_themes;
static float g_hue = 9, g_sat = 9;
static void theme(void *u, float hue, float sat) { (void)u; g_hue = hue; g_sat = sat; g_themes++; }
static void quiet(void *u, const char *m) { (void)u; (void)m; }

/* the hue (0..1) of an 0xRRGGBB through a theme */
static uint32_t turned(uint32_t rgb, float hue, float sat)
{
    float r = (rgb >> 16 & 255) / 255.0f, g = (rgb >> 8 & 255) / 255.0f, b = (rgb & 255) / 255.0f;
    pspexp_theme_px(&r, &g, &b, hue, sat);
    return (uint32_t)lroundf(r * 255) << 16 | (uint32_t)lroundf(g * 255) << 8 | (uint32_t)lroundf(b * 255);
}

int main(void)
{
    PspExpNullExecutor *nul;
    PspExpContext *ctx;
    uint32_t c;

    /* the turn: Midnight leaves everything; Crimson turns the accent blue red; Graphite greys it;
     * white, grey, the favourites' yellow and the Ready green stay */
    CHECK_EQ(turned(0x5aa9ff, 0.0f, 1.0f), 0x5aa9ffu);
    c = turned(0x5aa9ff, -0.62f, 1.0f);
    CHECK((c >> 16 & 255) > (c & 255) && (c >> 16 & 255) > (c >> 8 & 255));   /* red the strongest */
    c = turned(0x5aa9ff, 0.0f, 0.12f);
    CHECK(abs((int)(c >> 16 & 255) - (int)(c & 255)) < 40);                    /* near grey */
    CHECK_EQ(turned(0xffffff, -0.62f, 1.0f), 0xffffffu);
    CHECK_EQ(turned(0xa0a0a0, -0.62f, 1.0f), 0xa0a0a0u);
    CHECK_EQ(turned(0xffc766, -0.62f, 1.0f), 0xffc766u);
    CHECK_EQ(turned(0x4ade80, -0.62f, 1.0f), 0x4ade80u);

    /* the message: set before the connection, sent at it; set again, sent at once; after a reconnect, again */
    nul = pspexp_null_create(NULL);
    pspexp_null_runtime(nul)->ops.theme = theme;   /* (a PSP that turns its pictures) */
    pspexp_null_simulate_reconnect(nul);           /* (its greeting again, with THEME in it) */
    ctx = pspexp_create(pspexp_null_transport(nul), NULL);
    pspexp_set_log(ctx, quiet, NULL);
    CHECK_EQ(pspexp_theme(ctx, -0.62f, 1.0f), -1);   /* (not connected yet: kept) */
    CHECK_EQ(pspexp_connect(ctx, 1000), 0);
    pspexp_pump(ctx, 0);
    CHECK_EQ(g_themes, 1);
    CHECK(fabsf(g_hue + 0.62f) < 0.001f && fabsf(g_sat - 1.0f) < 0.001f);
    CHECK_EQ(pspexp_theme(ctx, 0.0f, 0.12f), 0);
    pspexp_pump(ctx, 0);
    CHECK_EQ(g_themes, 2);
    CHECK(fabsf(g_hue) < 0.001f && fabsf(g_sat - 0.12f) < 0.001f);
    pspexp_null_simulate_reconnect(nul);
    pspexp_pump(ctx, 0);
    pspexp_pump(ctx, 0);
    CHECK_EQ(pspexp_link_state(ctx), PSPEXP_LINK_CONNECTED);
    CHECK_EQ(g_themes, 3);
    CHECK(fabsf(g_sat - 0.12f) < 0.001f);
    pspexp_destroy(ctx);
    pspexp_null_destroy(nul);

    /* a PSP without it: nothing sent (it would answer BAD_OPCODE) */
    nul = pspexp_null_create(NULL);
    ctx = pspexp_create(pspexp_null_transport(nul), NULL);
    pspexp_set_log(ctx, quiet, NULL);
    CHECK_EQ(pspexp_connect(ctx, 1000), 0);
    CHECK_EQ(pspexp_theme(ctx, -0.62f, 1.0f), -1);
    pspexp_pump(ctx, 0);
    CHECK_EQ(pspexp_null_runtime(nul)->stats.protocol_errors, 0u);
    pspexp_destroy(ctx);
    pspexp_null_destroy(nul);
    return test_summary("theme");
}
