/*
 * The file manager's file work (tools/pspexp-term/files_ops.c), in a scratch
 * folder: listings (".." first, folders, then files, hidden ones hidden),
 * paths, sizes, names; copy, move and delete of a tree; conflicts; copying
 * a folder into itself refused.
 */
#include "test_util.h"
#include "../tools/pspexp-term/files_ops.h"
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

    /* rename and mkdir */
    fops_join(p, sizeof(p), root, "a"); fops_join(q, sizeof(q), root, "c");
    CHECK_EQ(fops_rename(p, q, first, sizeof(first)), 0);
    fops_join(p, sizeof(p), root, "new");
    CHECK_EQ(fops_mkdir(p, first, sizeof(first)), 0);
    CHECK_EQ(fops_rename(q, p, first, sizeof(first)), -1);   /* taken */

    /* clean up */
    for (i = 0; i < 1; i++) { job(FJOB_DELETE, "/tmp", "", strrchr(root, '/') + 1, &j); fjob_finish(&j); }
    CHECK(!exists(root));
    fops_free(&l);
    return test_summary("files_ops");
}
