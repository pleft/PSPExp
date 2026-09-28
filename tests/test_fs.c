/* The FILE channel end to end: libPSPExp asks, the null executor answers
 * from a temporary folder playing the memory stick (PSPEXP_MS0). */
#include "test_util.h"
#include "../host/libpspexp/pspexp.h"
#include "../host/executor-null/pspexp_null.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static void quiet_log(void *u, const char *msg) { (void)u; (void)msg; }

int main(void)
{
    char root[] = "/tmp/pspexp-test-fs-XXXXXX", p[512];
    static uint8_t big[PSPEXP_FS_CHUNK + 1000], back[PSPEXP_FS_CHUNK];
    PspExpFsEntry e[PSPEXP_FS_LIST_MAX];
    PspExpNullExecutor *nul;
    PspExpContext *ctx;
    uint64_t fr = 0, tot = 0;
    int more = -1, n, i, dirs = 0;
    FILE *f;

    CHECK(mkdtemp(root) != NULL);
    snprintf(p, sizeof(p), "%s/PSP", root); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/PSP/hello.txt", root);
    f = fopen(p, "w"); fputs("hello, stick\n", f); fclose(f);
    setenv("PSPEXP_MS0", root, 1);

    nul = pspexp_null_create(NULL);
    ctx = pspexp_create(pspexp_null_transport(nul), NULL);
    pspexp_set_log(ctx, quiet_log, NULL);
    CHECK_EQ(pspexp_fs_supported(ctx), 0);
    CHECK_EQ(pspexp_connect(ctx, 1000), 0);
    CHECK_EQ(pspexp_fs_supported(ctx), 1);

    /* listing and stat */
    n = pspexp_fs_list(ctx, "ms0:/", 0, e, PSPEXP_FS_LIST_MAX, &more);
    CHECK_EQ(n, 1);
    CHECK_EQ(more, 0);
    CHECK(strcmp(e[0].name, "PSP") == 0);
    CHECK(e[0].flags & PSPEXP_FS_DIR);
    CHECK_EQ(pspexp_fs_stat(ctx, "ms0:/PSP/hello.txt", e), 0);
    CHECK_EQ(e[0].size_lo, 13);
    CHECK_EQ(pspexp_fs_stat(ctx, "ms0:/nope", e), PSPEXP_FSERR_NOT_FOUND);

    /* reading */
    n = pspexp_fs_read(ctx, "ms0:/PSP/hello.txt", 7, back, sizeof(back));
    CHECK_EQ(n, 6);
    CHECK(memcmp(back, "stick\n", 6) == 0);

    /* writing more than a chunk: two calls */
    for (i = 0; i < (int)sizeof(big); i++) big[i] = (uint8_t)(i * 7);
    CHECK_EQ(pspexp_fs_mkdir(ctx, "ms0:/PSP/NEW"), 0);
    CHECK_EQ(pspexp_fs_mkdir(ctx, "ms0:/PSP/NEW"), PSPEXP_FSERR_EXISTS);
    CHECK_EQ(pspexp_fs_write(ctx, "ms0:/PSP/NEW/big.bin", 0, big, PSPEXP_FS_CHUNK, PSPEXP_FS_CREATE | PSPEXP_FS_TRUNCATE), PSPEXP_FS_CHUNK);
    CHECK_EQ(pspexp_fs_write(ctx, "ms0:/PSP/NEW/big.bin", PSPEXP_FS_CHUNK, big + PSPEXP_FS_CHUNK, 1000, 0), 1000);
    CHECK_EQ(pspexp_fs_stat(ctx, "ms0:/PSP/NEW/big.bin", e), 0);
    CHECK_EQ(e[0].size_lo, sizeof(big));
    CHECK_EQ(pspexp_fs_read(ctx, "ms0:/PSP/NEW/big.bin", 0, back, sizeof(back)), PSPEXP_FS_CHUNK);
    CHECK(memcmp(back, big, PSPEXP_FS_CHUNK) == 0);
    CHECK_EQ(pspexp_fs_read(ctx, "ms0:/PSP/NEW/big.bin", PSPEXP_FS_CHUNK, back, sizeof(back)), 1000);
    CHECK(memcmp(back, big + PSPEXP_FS_CHUNK, 1000) == 0);

    /* many entries: pages of PSPEXP_FS_LIST_MAX */
    for (i = 0; i < 70; i++) {
        snprintf(p, sizeof(p), "ms0:/PSP/NEW/d%02d", i);
        CHECK_EQ(pspexp_fs_mkdir(ctx, p), 0);
    }
    n = pspexp_fs_list(ctx, "ms0:/PSP/NEW", 0, e, PSPEXP_FS_LIST_MAX, &more);
    CHECK_EQ(n, PSPEXP_FS_LIST_MAX);
    CHECK_EQ(more, 1);
    for (i = 0; i < n; i++) dirs += (e[i].flags & PSPEXP_FS_DIR) != 0;
    n = pspexp_fs_list(ctx, "ms0:/PSP/NEW", PSPEXP_FS_LIST_MAX, e, PSPEXP_FS_LIST_MAX, &more);
    CHECK_EQ(n, 71 - PSPEXP_FS_LIST_MAX);
    CHECK_EQ(more, 0);
    for (i = 0; i < n; i++) dirs += (e[i].flags & PSPEXP_FS_DIR) != 0;
    CHECK_EQ(dirs, 70);

    /* rename, remove, the paths it refuses */
    CHECK_EQ(pspexp_fs_rename(ctx, "ms0:/PSP/hello.txt", "ms0:/PSP/NEW/hi.txt"), 0);
    CHECK_EQ(pspexp_fs_stat(ctx, "ms0:/PSP/NEW/hi.txt", e), 0);
    CHECK_EQ(pspexp_fs_remove(ctx, "ms0:/PSP/NEW"), PSPEXP_FSERR_NOT_EMPTY);
    CHECK_EQ(pspexp_fs_remove(ctx, "ms0:/PSP/NEW/hi.txt"), 0);
    CHECK_EQ(pspexp_fs_remove(ctx, "ms0:/PSP/NEW/d00"), 0);
    CHECK_EQ(pspexp_fs_stat(ctx, "ms0:/PSP/NEW/d00", e), PSPEXP_FSERR_NOT_FOUND);
    CHECK_EQ(pspexp_fs_stat(ctx, "ms0:/PSP/../../etc", e), PSPEXP_FSERR_PATH);
    CHECK_EQ(pspexp_fs_stat(ctx, "/etc/passwd", e), PSPEXP_FSERR_PATH);
    CHECK_EQ(pspexp_fs_info(ctx, "ms0:/", &fr, &tot), 0);
    CHECK(tot > 0 && fr <= tot);

    /* the clock (CLOCK_SET): moved only past min_diff; report-only leaves it */
    {
        PspExpClockStatus cs;
        uint64_t now = (uint64_t)time(NULL), after;
        CHECK_EQ(pspexp_clock_supported(ctx), 1);
        CHECK_EQ(pspexp_clock_sync(ctx, now + 30, 0, 60, &cs), 0);          /* 30 s off: close enough */
        CHECK_EQ(pspexp_clock_sync(ctx, now + 100000, 0, 60, &cs), 1);      /* a day off: set */
        after = (uint64_t)cs.after_lo | ((uint64_t)cs.after_hi << 32);
        CHECK(after >= now + 100000 - 2 && after <= now + 100000 + 5);
        CHECK_EQ(pspexp_clock_sync(ctx, now, 0, PSPEXP_CLOCK_REPORT_ONLY, &cs), 0);
        CHECK(((uint64_t)cs.before_lo | ((uint64_t)cs.before_hi << 32)) >= now + 100000 - 2);   /* still moved */
        CHECK_EQ(pspexp_clock_sync(ctx, now, 0, 0, &cs), 1);                 /* 0: always */
    }

    pspexp_destroy(ctx);
    pspexp_null_destroy(nul);
    snprintf(p, sizeof(p), "rm -rf '%s'", root);
    CHECK_EQ(system(p), 0);
    return test_summary("test_fs");
}
