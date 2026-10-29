/* HOME pressed twice (host/libpspexp, home_twice): the first press is held back and the PSP asked
 * for its "HOME again to quit" bar; a second within 3 s goes through and the bar goes; a PSP that
 * cannot draw the bar passes the first press. Through the null executor's runtime, as test_runtime. */
#include "test_util.h"
#include "../host/libpspexp/pspexp.h"
#include "../host/executor-null/pspexp_null.h"
#include "../runtime/pspexp_runtime.h"
#include <string.h>

static int g_kind = -1, g_hints;
static uint32_t g_ms = 99;
static void hint(void *u, int kind, uint32_t ms) { (void)u; g_kind = kind; g_ms = ms; g_hints++; }
static void quiet(void *u, const char *m) { (void)u; (void)m; }

static uint32_t press(PspExpNullExecutor *n, PspExpContext *ctx, uint32_t b)
{
    pspexp_null_push_input(n, b, 128, 128);
    pspexp_pump(ctx, 0);
    pspexp_pump(ctx, 0);   /* (the hint's message to the runtime, then back) */
    return pspexp_input(ctx)->buttons;
}

int main(void)
{
    PspExpNullExecutor *nul = pspexp_null_create(NULL);
    PspExpContext *ctx;
    pspexp_null_runtime(nul)->ops.osd_hint = hint;   /* (a PSP that draws the bar) */
    pspexp_null_simulate_reconnect(nul);              /* (its greeting again, with the bar in it) */
    ctx = pspexp_create(pspexp_null_transport(nul), NULL);
    pspexp_set_log(ctx, quiet, NULL);
    CHECK_EQ(pspexp_connect(ctx, 1000), 0);

    /* first press: held back, the bar asked for 3 s */
    CHECK_EQ(press(nul, ctx, PSPEXP_BUTTON_HOME) & PSPEXP_BUTTON_HOME, 0u);
    CHECK_EQ(g_kind, PSPEXP_HINT_HOME);
    CHECK_EQ(g_ms, 3000u);
    CHECK(pspexp_osd_until(ctx) > 0);
    /* held: still held back; other buttons with it go through */
    CHECK_EQ(press(nul, ctx, PSPEXP_BUTTON_HOME | PSPEXP_BUTTON_CROSS), (uint32_t)PSPEXP_BUTTON_CROSS);
    CHECK_EQ(press(nul, ctx, 0), 0u);
    /* second press: through, the bar gone */
    CHECK_EQ(press(nul, ctx, PSPEXP_BUTTON_HOME) & PSPEXP_BUTTON_HOME, (uint32_t)PSPEXP_BUTTON_HOME);
    CHECK_EQ(g_ms, 0u);
    CHECK_EQ(g_hints, 2);
    CHECK_EQ(press(nul, ctx, 0), 0u);
    /* and the next press starts again: held back */
    CHECK_EQ(press(nul, ctx, PSPEXP_BUTTON_HOME) & PSPEXP_BUTTON_HOME, 0u);
    CHECK_EQ(g_hints, 3);
    press(nul, ctx, 0);
    pspexp_destroy(ctx);
    pspexp_null_destroy(nul);

    /* a PSP without the bar: the first press goes through */
    nul = pspexp_null_create(NULL);
    ctx = pspexp_create(pspexp_null_transport(nul), NULL);
    pspexp_set_log(ctx, quiet, NULL);
    CHECK_EQ(pspexp_connect(ctx, 1000), 0);
    CHECK_EQ(press(nul, ctx, PSPEXP_BUTTON_HOME) & PSPEXP_BUTTON_HOME, (uint32_t)PSPEXP_BUTTON_HOME);
    pspexp_destroy(ctx);
    pspexp_null_destroy(nul);
    return test_summary("home");
}
