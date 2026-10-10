/* The launcher's cover and disc caches (tools/pspexp-term/launcher_gfx.c),
 * drawn on the null executor. */
#include "test_util.h"
#include "../host/libpspexp/pspexp.h"
#include "../host/executor-null/pspexp_null.h"
#include "../tools/pspexp-term/launcher_gfx.h"
#include <string.h>

static void quiet_log(void *u, const char *m) { (void)u; (void)m; }

int main(void)
{
    PspExpNullExecutor *nul = pspexp_null_create(NULL);
    PspExpContext *ctx = pspexp_create(pspexp_null_transport(nul), NULL);
    CoverSpec keep, other;
    char key[32];
    int k, distinct = 1, first_bad = -1;
    CHECK(ctx != NULL);
    pspexp_set_log(ctx, quiet_log, NULL);
    CHECK_EQ(pspexp_connect(ctx, 1000), 0);
    CHECK_EQ(gfx_init(ctx, PSPEXP_LAUNCHER_ASSET_DIR), 0);
    /* The selection's disc and cover stay in use while the shelf scrolls past
     * many others: no other picture may be given their texture id. The id
     * pools turn over after 40 discs and 32 covers; an id still held was
     * given again, the kept picture overwritten with another game's and
     * then released (a wrong cover, a selected disc that did not grow). */
    memset(&keep, 0, sizeof(keep)); keep.key = "keep"; keep.title = "Kept game"; keep.round = 1;
    memset(&other, 0, sizeof(other)); other.title = "Another game"; other.round = 1; other.key = key;
    for (k = 0; k < 120; k++) {
        gfx_frame(ctx);
        snprintf(key, sizeof(key), "game-%d", k);
        pspexp_begin_frame(ctx);
        gfx_disc(&keep, 100, 100, 1, 255);
        gfx_disc(&other, 200, 100, 0, 255);
        gfx_cover(&keep, 0, 0, 255);
        gfx_cover(&other, 0, 130, 255);
        pspexp_present(ctx);
        pspexp_pump(ctx, 0);
        if (!gfx_cache_ids_distinct() && distinct) { distinct = 0; first_bad = k; }
    }
    if (!distinct) fprintf(stderr, "two cached pictures share a texture id from game %d on\n", first_bad);
    CHECK(distinct);
    CHECK_EQ(pspexp_null_runtime(nul)->stats.protocol_errors, 0);
    pspexp_destroy(ctx);
    pspexp_null_destroy(nul);
    return test_summary("test_launcher_gfx");
}
