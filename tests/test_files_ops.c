/*
 * The file manager's file work (tools/pspexp-term/files_ops.c), in a scratch
 * folder: listings (".." first, folders, then files, hidden ones hidden),
 * paths, sizes, names; copy, move and delete of a tree; conflicts; copying
 * a folder into itself refused; archives packed and unpacked by bsdtar.
 */
#include "test_util.h"
#include "../tools/pspexp-term/files_ops.h"
#include "../tools/pspexp-term/files_psp.h"
#include "../host/executor-null/pspexp_null.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static int exists(const char *path) { struct stat st; return lstat(path, &st) == 0; }

static int same_file(const char *a, const char *b)
{
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    int ca, cb, same = x && y;
    while (same) { ca = fgetc(x); cb = fgetc(y); if (ca != cb) same = 0; if (ca == EOF) break; }
    if (x) fclose(x);
    if (y) fclose(y);
    return same;
}

static void quiet(void *u, const char *m) { (void)u; (void)m; }

static void job(int kind, const char *src, const char *dst, const char *name, FJob *j)
{
    memset(j, 0, sizeof(*j));
    j->kind = kind;
    snprintf(j->src, sizeof(j->src), "%s", src);
    snprintf(j->dst, sizeof(j->dst), "%s", dst);
    j->names = calloc(1, 256);
    snprintf(j->names[0], 256, "%s", name);
    j->nnames = 1;
    fjob_run(j);
}

int main(void)
{
    char root[] = "/tmp/pspexp-files-XXXXXX", p[1024], q[1024], s[32], first[256];
    FList l = { 0 };
    FJob j;
    int i;
    if (!mkdtemp(root)) return 1;
    fops_join(p, sizeof(p), root, "a"); mkdir(p, 0755);
    fops_join(p, sizeof(p), root, "a/sub"); mkdir(p, 0755);
    fops_join(p, sizeof(p), root, "a/one.txt"); put(p, "hello");
    fops_join(p, sizeof(p), root, "a/sub/two.txt"); put(p, "world!");
    fops_join(p, sizeof(p), root, "b"); mkdir(p, 0755);
    fops_join(p, sizeof(p), root, "Zed.txt"); put(p, "z");
    fops_join(p, sizeof(p), root, ".hidden"); put(p, "h");

    CHECK_EQ(fops_list(root, 0, FSORT_NAME, &l), 0);
    CHECK_EQ(l.n, 4);   /* .., a, b, Zed.txt */
    if (l.n == 4) {
        CHECK(l.e[0].is_up && !strcmp(l.e[0].name, ".."));
        CHECK(!strcmp(l.e[1].name, "a") && l.e[1].is_dir);
        CHECK(!strcmp(l.e[2].name, "b") && l.e[2].is_dir);
        CHECK(!strcmp(l.e[3].name, "Zed.txt") && !l.e[3].is_dir && l.e[3].size == 1);
    }
    CHECK_EQ(fops_list(root, 1, FSORT_NAME, &l), 0);
    CHECK_EQ(l.n, 5);
    CHECK_EQ(fops_list("/", 0, FSORT_NAME, &l), 0);
    CHECK(l.n > 0 && !l.e[0].is_up);   /* no ".." at the root */

    strcpy(p, "/a/b/c"); CHECK(!strcmp(fops_parent(p), "c") && !strcmp(p, "/a/b"));
    strcpy(p, "/x"); fops_parent(p); CHECK(!strcmp(p, "/"));
    fops_join(p, sizeof(p), "/", "etc"); CHECK(!strcmp(p, "/etc"));
    fops_human(512, s, sizeof(s)); CHECK(!strcmp(s, "512 B"));
    fops_human(1536, s, sizeof(s)); CHECK(!strcmp(s, "1.5 KB"));
    fops_human(50ull * 1024 * 1024, s, sizeof(s)); CHECK(!strcmp(s, "50 MB"));
    CHECK(fops_name_ok("x.txt") && !fops_name_ok("a/b") && !fops_name_ok("..") && !fops_name_ok(""));

    /* copy a tree into b */
    fops_join(q, sizeof(q), root, "b");
    job(FJOB_COPY, root, q, "a", &j);
    CHECK(!j.error[0]);
    CHECK_EQ(j.files_total, 2); CHECK_EQ(j.files_done, 2);
    CHECK(j.bytes_total == 11 && j.bytes_done == 11);
    fops_join(p, sizeof(p), root, "b/a/sub/two.txt"); CHECK(exists(p));
    fops_join(p, sizeof(p), root, "a/one.txt"); CHECK(exists(p));   /* the original stays */
    fjob_finish(&j);

    /* conflicts: copying a again would replace b/a */
    {
        char (*names)[256] = calloc(1, 256);
        strcpy(names[0], "a");
        CHECK_EQ(fops_conflicts(q, names, 1, first, sizeof(first)), 1);
        CHECK(!strcmp(first, "a"));
        free(names);
    }
    /* a folder into itself: refused */
    fops_join(p, sizeof(p), root, "a");
    job(FJOB_COPY, root, p, "a", &j);
    CHECK(strstr(j.error, "inside itself") != NULL);
    fjob_finish(&j);

    /* move Zed.txt into b, then delete b */
    job(FJOB_MOVE, root, q, "Zed.txt", &j);
    CHECK(!j.error[0]);
    fops_join(p, sizeof(p), root, "Zed.txt"); CHECK(!exists(p));
    fops_join(p, sizeof(p), root, "b/Zed.txt"); CHECK(exists(p));
    fjob_finish(&j);
    job(FJOB_DELETE, root, "", "b", &j);
    CHECK(!j.error[0]);
    CHECK(!exists(q));
    fjob_finish(&j);

    /* archives: a folder packed into a .zip, listed, unpacked elsewhere, the same files back */
    CHECK(fops_is_archive("x.zip") && fops_is_archive("g.TAR.GZ") && fops_is_archive("a.7z") && !fops_is_archive("zip") && !fops_is_archive("a.txt"));
    fops_archive_stem("game.tar.gz", first, sizeof(first)); CHECK(!strcmp(first, "game"));
    fops_archive_stem("Some.Thing.zip", first, sizeof(first)); CHECK(!strcmp(first, "Some.Thing"));
    if (system("command -v bsdtar >/dev/null 2>&1") == 0) {
        char out[4096];
        memset(&j, 0, sizeof(j));
        j.kind = FJOB_COMPRESS;
        snprintf(j.src, sizeof(j.src), "%s", root); snprintf(j.dst, sizeof(j.dst), "%s", root);
        snprintf(j.out, sizeof(j.out), "a.zip");
        j.names = calloc(1, 256); snprintf(j.names[0], 256, "a"); j.nnames = 1;
        fjob_run(&j);
        CHECK(!j.error[0]);
        CHECK(j.files_total >= 4 && j.files_done == j.files_total);   /* a, a/one.txt, a/sub, a/sub/two.txt */
        fjob_finish(&j);
        fops_join(p, sizeof(p), root, "a.zip");
        CHECK_EQ(fops_archive_list(p, out, sizeof(out)), 0);
        CHECK(strstr(out, "a/sub/two.txt") != NULL);
        fops_join(q, sizeof(q), root, "unpacked");
        job(FJOB_EXTRACT, root, q, "a.zip", &j);
        CHECK(!j.error[0]);
        CHECK_EQ(j.files_total, 2); CHECK_EQ(j.files_done, 2);   /* files only */
        fjob_finish(&j);
        { char a[1024], b[1024];
          fops_join(a, sizeof(a), root, "a/sub/two.txt"); fops_join(b, sizeof(b), root, "unpacked/a/sub/two.txt");
          CHECK(same_file(a, b)); }
        /* one folder in it, unpacked where its name is free: that folder, not a/a */
        fops_join(q, sizeof(q), root, "two"); mkdir(q, 0755);
        fops_join(q, sizeof(q), root, "two/a");
        job(FJOB_EXTRACT, root, q, "a.zip", &j);
        CHECK(!j.error[0]);
        fjob_finish(&j);
        fops_join(p, sizeof(p), root, "two/a/sub/two.txt"); CHECK(exists(p));
        fops_join(p, sizeof(p), root, "two/a/a"); CHECK(!exists(p));
        job(FJOB_DELETE, root, "", "two", &j); fjob_finish(&j);
        fops_join(p, sizeof(p), root, "a.zip");
        /* not an archive: a reason, no folder left half made */
        fops_join(p, sizeof(p), root, "bad.zip"); put(p, "not a zip");
        fops_join(q, sizeof(q), root, "bad");
        job(FJOB_EXTRACT, root, q, "bad.zip", &j);
        CHECK(j.error[0] != 0);
        fjob_finish(&j);
        CHECK(!exists(q));
        CHECK(fops_archive_list(p, out, sizeof(out)) < 0 && out[0]);
        job(FJOB_DELETE, root, "", "unpacked", &j); fjob_finish(&j);
        job(FJOB_DELETE, root, "", "a.zip", &j); fjob_finish(&j);
        job(FJOB_DELETE, root, "", "bad.zip", &j); fjob_finish(&j);
    } else printf("files_ops: no bsdtar here, archives not tried\n");

    /* rename and mkdir */
    fops_join(p, sizeof(p), root, "a"); fops_join(q, sizeof(q), root, "c");
    CHECK_EQ(fops_rename(p, q, first, sizeof(first)), 0);
    fops_join(p, sizeof(p), root, "new");
    CHECK_EQ(fops_mkdir(p, first, sizeof(first)), 0);
    CHECK_EQ(fops_rename(q, p, first, sizeof(first)), -1);   /* taken */

    /* the PSP's memory stick as a drive: the null executor plays the PSP,
     * root/stick its memory stick; a job on its own thread goes through the
     * broker, one run here calls directly */
    {
        PspExpNullExecutor *nul;
        PspExpContext *ctx;
        char stick[1024], tree[1024], big[1024], sub[1024];
        FILE *f;
        uint64_t fr = 0, tot = 0;
        int k, turns = 0;
        fops_join(stick, sizeof(stick), root, "stick"); mkdir(stick, 0755);
        fops_join(tree, sizeof(tree), root, "tree"); mkdir(tree, 0755);
        fops_join(sub, sizeof(sub), tree, "sub"); mkdir(sub, 0755);
        fops_join(p, sizeof(p), tree, "x.txt"); put(p, "x");
        fops_join(p, sizeof(p), tree, "empty.txt"); put(p, "");
        fops_join(p, sizeof(p), sub, "y.txt"); put(p, "why");
        fops_join(big, sizeof(big), tree, "big.bin");
        f = fopen(big, "wb");
        for (k = 0; k < 200000; k++) fputc((k * 31) & 0xff, f);
        fclose(f);
        setenv("PSPEXP_MS0", stick, 1);
        nul = pspexp_null_create(NULL);
        ctx = pspexp_create(pspexp_null_transport(nul), NULL);
        pspexp_set_log(ctx, quiet, NULL);
        CHECK_EQ(pspexp_connect(ctx, 1000), 0);
        files_psp_init(ctx);
        CHECK(files_psp_available());
        CHECK(fops_is_remote("ms0:/PSP") && !fops_is_remote("/ms0:"));
        CHECK(fops_is_root("ms0:/") && fops_is_root("/") && !fops_is_root("ms0:/PSP"));
        snprintf(p, sizeof(p), "ms0:/PSP");
        CHECK(!strcmp(fops_parent(p), "PSP") && !strcmp(p, "ms0:/"));
        snprintf(p, sizeof(p), "ms0:/PSP/GAME");
        CHECK(!strcmp(fops_parent(p), "GAME") && !strcmp(p, "ms0:/PSP"));

        /* board -> PSP, on a thread */
        memset(&j, 0, sizeof(j));
        j.kind = FJOB_COPY;
        snprintf(j.src, sizeof(j.src), "%s", root);
        snprintf(j.dst, sizeof(j.dst), "ms0:/");
        j.names = calloc(1, 256); snprintf(j.names[0], 256, "tree"); j.nnames = 1;
        CHECK_EQ(fjob_start(&j), 0);
        while (!j.done && turns < 100000) { files_psp_service(ctx, 10000); turns++; }
        fjob_finish(&j);
        CHECK(!j.error[0]);
        CHECK_EQ(j.files_done, 4);
        CHECK_EQ(j.bytes_total, 200000 + 1 + 3);
        fops_join(p, sizeof(p), stick, "tree/big.bin"); CHECK(same_file(p, big));
        fops_join(p, sizeof(p), stick, "tree/empty.txt"); CHECK(exists(p));
        fops_join(p, sizeof(p), stick, "tree/sub/y.txt"); CHECK(exists(p));

        /* listings: no ".." at the top of the stick */
        CHECK_EQ(fops_list("ms0:/", 0, FSORT_NAME, &l), 0);
        CHECK_EQ(l.n, 1); CHECK(!strcmp(l.e[0].name, "tree") && l.e[0].is_dir);
        CHECK_EQ(fops_list("ms0:/tree", 0, FSORT_SIZE, &l), 0);
        CHECK_EQ(l.n, 5);
        CHECK(l.e[0].is_up && !strcmp(l.e[1].name, "sub") && !strcmp(l.e[2].name, "big.bin") && l.e[2].size == 200000);
        CHECK_EQ(fops_list("ms0:/nope", 0, FSORT_NAME, &l), -1);
        CHECK(!strcmp(fops_strerror(errno), strerror(ENOENT)));
        CHECK(files_psp_space("ms0:/", &fr, &tot) == 0 && tot > 0);

        /* PSP -> board, here */
        fops_join(q, sizeof(q), root, "back"); mkdir(q, 0755);
        job(FJOB_COPY, "ms0:/", q, "tree", &j);
        CHECK(!j.error[0]);
        fjob_finish(&j);
        fops_join(p, sizeof(p), q, "tree/big.bin"); CHECK(same_file(p, big));
        CHECK_EQ(fops_conflicts("ms0:/", (char (*)[256])"tree", 1, first, sizeof(first)), 1);

        /* on the stick: mkdir, rename, move within it, then onto the board */
        CHECK_EQ(fops_mkdir("ms0:/NEW", first, sizeof(first)), 0);
        CHECK_EQ(fops_mkdir("ms0:/NEW", first, sizeof(first)), -1);
        CHECK_EQ(fops_rename("ms0:/tree/x.txt", "ms0:/tree/z.txt", first, sizeof(first)), 0);
        job(FJOB_MOVE, "ms0:/", "ms0:/NEW", "tree", &j);
        CHECK(!j.error[0]);
        fjob_finish(&j);
        fops_join(p, sizeof(p), stick, "NEW/tree/z.txt"); CHECK(exists(p));
        fops_join(q, sizeof(q), root, "moved"); mkdir(q, 0755);
        job(FJOB_MOVE, "ms0:/NEW", q, "tree", &j);
        CHECK(!j.error[0]);
        fjob_finish(&j);
        fops_join(p, sizeof(p), q, "tree/big.bin"); CHECK(same_file(p, big));
        fops_join(p, sizeof(p), stick, "NEW/tree"); CHECK(!exists(p));

        /* board -> PSP move, then delete on the stick */
        job(FJOB_MOVE, q, "ms0:/NEW", "tree", &j);
        CHECK(!j.error[0]);
        fjob_finish(&j);
        fops_join(p, sizeof(p), q, "tree"); CHECK(!exists(p));
        job(FJOB_DELETE, "ms0:/", "", "NEW", &j);
        CHECK(!j.error[0]);
        CHECK_EQ(j.files_done, 4);
        fjob_finish(&j);
        CHECK_EQ(fops_list("ms0:/", 0, FSORT_NAME, &l), 0);
        CHECK_EQ(l.n, 0);

        /* the link gone: every call says so, nothing hangs */
        pspexp_set_transport(ctx, NULL);
        CHECK(!files_psp_available());
        CHECK_EQ(fops_list("ms0:/", 0, FSORT_NAME, &l), -1);
        CHECK(!strcmp(fops_strerror(errno), "the PSP did not answer"));
        pspexp_destroy(ctx);
        pspexp_null_destroy(nul);
    }

    /* drives from a mounts table: an Omega's SD card, a desktop's stick
     * (label with a space), two plain USB mounts; system ones left out */
    {
        FDrive d[FOPS_DRIVES];
        const char *mounts =
            "overlayfs:/overlay / overlay rw 0 0\n"
            "proc /proc proc rw 0 0\n"
            "/dev/mmcblk0p1 /boot vfat rw 0 0\n"
            "/dev/mmcblk0p1 /mnt/mmcblk0p1 vfat rw 0 0\n"
            "/dev/mmcblk0p1 /mnt/mmcblk0p1 vfat rw 0 0\n"
            "/dev/sda1 /media/user/MY\\040STICK vfat rw 0 0\n"
            "/dev/sdb1 /mnt/usb vfat rw 0 0\n"
            "/dev/sdc1 /mnt/usb2 vfat rw 0 0\n"
            "/dev/loop0 /snap/core squashfs ro 0 0\n"
            "tmpfs /tmp tmpfs rw 0 0\n"
            "/dev/mmcblk0p2 /var/log ext4 rw 0 0\n"
            "/dev/root /rom squashfs ro 0 0\n"
            "/dev/mtdblock6 /overlay jffs2 rw 0 0\n"
            "/dev/mtdblock7 /mnt/mtdblock7 jffs2 rw 0 0";
        int n = fops_board_drives(d, FOPS_DRIVES, mounts);
        CHECK_EQ(n, 5);
        CHECK(!strcmp(d[0].root, "/") && !strcmp(d[0].name, "Board"));
        CHECK(!strcmp(d[1].root, "/mnt/mmcblk0p1") && !strcmp(d[1].name, "SD card"));
        CHECK(!strcmp(d[2].root, "/media/user/MY STICK") && !strcmp(d[2].name, "MY STICK") && !strcmp(d[2].icon, "usb"));
        CHECK(!strcmp(d[3].name, "USB") && !strcmp(d[4].name, "usb2"));
        CHECK_EQ(fops_drive_of(d, n, "/mnt/usb2/games"), 4);
        CHECK_EQ(fops_drive_of(d, n, "/mnt/usb"), 3);
        CHECK_EQ(fops_drive_of(d, n, "/mnt/usbx"), 0);
        CHECK_EQ(fops_drive_of(d, n, "/home/user"), 0);
        CHECK_EQ(fops_drive_of(d, n, "ms0:/PSP"), -1);
        CHECK(fops_board_drives(d, FOPS_DRIVES, NULL) >= 1);   /* this machine's: "/" at least */
    }

    /* clean up */
    for (i = 0; i < 1; i++) { job(FJOB_DELETE, "/tmp", "", strrchr(root, '/') + 1, &j); fjob_finish(&j); }
    CHECK(!exists(root));
    fops_free(&l);
    return test_summary("files_ops");
}
