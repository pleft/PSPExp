/* The file manager's work on the file system: see files_ops.h */
#define _GNU_SOURCE
#include "files_ops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- listings ---------------------------------------------------------------- */

static int g_sort;

static int cmp(const void *a, const void *b)
{
    const FEntry *x = a, *y = b;
    if (x->is_up != y->is_up) return y->is_up - x->is_up;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    if (g_sort == FSORT_SIZE && x->size != y->size) return x->size < y->size ? 1 : -1;       /* largest first */
    if (g_sort == FSORT_DATE && x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;   /* newest first */
    return strcasecmp(x->name, y->name);
}

int fops_list(const char *dir, int show_hidden, int sort, FList *out)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    out->n = 0;
    if (!d) return -1;
    if (strcmp(dir, "/")) {
        if (out->cap < 16) { FEntry *e = realloc(out->e, sizeof(FEntry) * 16); if (!e) { closedir(d); return -1; } out->e = e; out->cap = 16; }
        memset(&out->e[0], 0, sizeof(FEntry));
        strcpy(out->e[0].name, "..");
        out->e[0].is_dir = out->e[0].is_up = 1;
        out->n = 1;
    }
    while ((de = readdir(d))) {
        FEntry *e;
        struct stat st, lst;
        char p[FOPS_PATH];
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!show_hidden && de->d_name[0] == '.') continue;
        if (out->n == out->cap) {
            int nc = out->cap ? out->cap * 2 : 64;
            FEntry *ne = realloc(out->e, sizeof(FEntry) * (size_t)nc);
            if (!ne) break;
            out->e = ne; out->cap = nc;
        }
        e = &out->e[out->n];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        fops_join(p, sizeof(p), dir, de->d_name);
        if (lstat(p, &lst) == 0) {
            e->is_link = S_ISLNK(lst.st_mode);
            if (e->is_link && stat(p, &st) == 0) lst = st;   /* a link shows what it points to */
            e->is_dir = S_ISDIR(lst.st_mode);
            e->size = (uint64_t)lst.st_size;
            e->mtime = lst.st_mtime;
            e->mode = lst.st_mode;
        }
        out->n++;
    }
    closedir(d);
    g_sort = sort;
    qsort(out->e, (size_t)out->n, sizeof(FEntry), cmp);
    return 0;
}

void fops_free(FList *l) { free(l->e); l->e = NULL; l->n = l->cap = 0; }

void fops_join(char *out, size_t n, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    snprintf(out, n, "%s%s%s", dir, l && dir[l - 1] == '/' ? "" : "/", name);
}

const char *fops_parent(char *path)
{
    static char last[256];
    char *s = strrchr(path, '/');
    last[0] = 0;
    if (!s) return last;
    snprintf(last, sizeof(last), "%s", s + 1);
    if (s == path) path[1] = 0;   /* "/x" -> "/" */
    else *s = 0;
    return last;
}

void fops_human(uint64_t b, char *out, size_t n)
{
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    if (i == 0) snprintf(out, n, "%llu B", (unsigned long long)b);
    else snprintf(out, n, v < 10 ? "%.1f %s" : "%.0f %s", v, u[i]);
}

int fops_name_ok(const char *name)
{
    return name && name[0] && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..");
}

int fops_mkdir(const char *path, char *err, size_t en)
{
    if (mkdir(path, 0755) == 0) return 0;
    snprintf(err, en, "%s", strerror(errno));
    return -1;
}

int fops_rename(const char *from, const char *to, char *err, size_t en)
{
    struct stat st;
    if (lstat(to, &st) == 0) { snprintf(err, en, "that name is taken"); return -1; }
    if (rename(from, to) == 0) return 0;
    snprintf(err, en, "%s", strerror(errno));
    return -1;
}

int fops_conflicts(const char *dst, char (*names)[256], int n, char *first, size_t fn)
{
    int i, c = 0;
    struct stat st;
    char p[FOPS_PATH];
    if (first && fn) first[0] = 0;
    for (i = 0; i < n; i++) {
        fops_join(p, sizeof(p), dst, names[i]);
        if (lstat(p, &st) == 0) { if (!c && first) snprintf(first, fn, "%s", names[i]); c++; }
    }
    return c;
}

/* ---- jobs -------------------------------------------------------------------- */

static void job_err(FJob *j, const char *what, const char *path)
{
    if (!j->error[0]) snprintf(j->error, sizeof(j->error), "%s %s: %s", what, path, strerror(errno));
}

/* the size and number of files below path (a file: itself) */
static void measure(FJob *j, const char *path)
{
    struct stat st;
    if (j->cancel || lstat(path, &st) < 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *de;
        if (!d) return;
        while ((de = readdir(d))) {
            char p[FOPS_PATH];
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            fops_join(p, sizeof(p), path, de->d_name);
            measure(j, p);
        }
        closedir(d);
    } else {
        j->bytes_total += S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
        j->files_total++;
    }
}

static int remove_tree(FJob *j, const char *path)
{
    struct stat st;
    if (j->cancel) return -1;
    if (lstat(path, &st) < 0) { job_err(j, "cannot read", path); return -1; }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *de;
        int rc = 0;
        if (!d) { job_err(j, "cannot open", path); return -1; }
        while ((de = readdir(d)) && !rc) {
            char p[FOPS_PATH];
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            fops_join(p, sizeof(p), path, de->d_name);
            rc = remove_tree(j, p);
        }
        closedir(d);
        if (rc) return rc;
        if (rmdir(path) < 0) { job_err(j, "cannot delete", path); return -1; }
        return 0;
    }
    snprintf(j->current, sizeof(j->current), "%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    if (unlink(path) < 0) { job_err(j, "cannot delete", path); return -1; }
    j->files_done++;
    j->bytes_done += S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
    return 0;
}

static int copy_file(FJob *j, const char *from, const char *to, const struct stat *st)
{
    enum { BUF = 256 * 1024 };
    char *buf;
    int in, out, rc = 0;
    ssize_t n = 0;
    snprintf(j->current, sizeof(j->current), "%s", strrchr(from, '/') ? strrchr(from, '/') + 1 : from);
    if (!(buf = malloc(BUF))) { snprintf(j->error, sizeof(j->error), "out of memory"); return -1; }
    if ((in = open(from, O_RDONLY)) < 0) { job_err(j, "cannot read", from); free(buf); return -1; }
    if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC, st->st_mode & 07777)) < 0) { job_err(j, "cannot write", to); close(in); free(buf); return -1; }
    while (!j->cancel && (n = read(in, buf, BUF)) > 0) {
        char *p = buf;
        while (n > 0) {
            ssize_t w = write(out, p, (size_t)n);
            if (w < 0) { if (errno == EINTR) continue; job_err(j, "cannot write", to); rc = -1; break; }
            p += w; n -= w; j->bytes_done += (uint64_t)w;
        }
        if (rc) break;
    }
    if (n < 0) { job_err(j, "cannot read", from); rc = -1; }
    if (close(out) < 0 && !rc) { job_err(j, "cannot write", to); rc = -1; }
    close(in);
    free(buf);
    if (j->cancel) rc = -1;
    if (rc) unlink(to);   /* no half files */
    else j->files_done++;
    return rc;
}

static int copy_tree(FJob *j, const char *from, const char *to)
{
    struct stat st;
    if (j->cancel) return -1;
    if (lstat(from, &st) < 0) { job_err(j, "cannot read", from); return -1; }
    if (S_ISLNK(st.st_mode)) {
        char target[FOPS_PATH];
        ssize_t l = readlink(from, target, sizeof(target) - 1);
        if (l < 0) { job_err(j, "cannot read", from); return -1; }
        target[l] = 0;
        unlink(to);
        if (symlink(target, to) < 0) { job_err(j, "cannot write", to); return -1; }
        j->files_done++;
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *de;
        int rc = 0;
        if (mkdir(to, st.st_mode & 07777) < 0 && errno != EEXIST) { job_err(j, "cannot create", to); return -1; }
        if (!(d = opendir(from))) { job_err(j, "cannot open", from); return -1; }
        while ((de = readdir(d)) && !rc) {
            char a[FOPS_PATH], b[FOPS_PATH];
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            fops_join(a, sizeof(a), from, de->d_name);
            fops_join(b, sizeof(b), to, de->d_name);
            rc = copy_tree(j, a, b);
        }
        closedir(d);
        return rc;
    }
    if (!S_ISREG(st.st_mode)) { j->files_done++; return 0; }   /* devices, pipes: skipped */
    return copy_file(j, from, to, &st);
}

/* dst inside src (copying a folder into itself) */
static int inside(const char *src, const char *dst)
{
    size_t l = strlen(src);
    return !strncmp(dst, src, l) && (dst[l] == '/' || dst[l] == 0);
}

void fjob_run(FJob *j)
{
    int i;
    j->error[0] = 0;
    for (i = 0; i < j->nnames && !j->cancel; i++) {
        char p[FOPS_PATH];
        fops_join(p, sizeof(p), j->src, j->names[i]);
        measure(j, p);
    }
    for (i = 0; i < j->nnames && !j->cancel && !j->error[0]; i++) {
        char a[FOPS_PATH], b[FOPS_PATH];
        fops_join(a, sizeof(a), j->src, j->names[i]);
        if (j->kind == FJOB_DELETE) { remove_tree(j, a); continue; }
        fops_join(b, sizeof(b), j->dst, j->names[i]);
        if (!strcmp(a, b)) { snprintf(j->error, sizeof(j->error), "%s is already there", j->names[i]); break; }
        if (inside(a, b)) { snprintf(j->error, sizeof(j->error), "cannot put %s inside itself", j->names[i]); break; }
        if (j->kind == FJOB_MOVE) {
            struct stat st;
            if (lstat(b, &st) == 0) remove_tree(j, b);   /* the view asked before replacing */
            if (rename(a, b) == 0) { j->files_done++; continue; }
            if (errno != EXDEV) { job_err(j, "cannot move", a); break; }
            /* another file system: copy, then delete the original */
            if (copy_tree(j, a, b) == 0) {
                uint64_t keep = j->bytes_done; int kf = j->files_done;
                remove_tree(j, a);
                j->bytes_done = keep; j->files_done = kf;   /* progress is the copy's */
            }
        } else copy_tree(j, a, b);
    }
    if (j->cancel && !j->error[0]) snprintf(j->error, sizeof(j->error), "cancelled");
    j->done = 1;
}

static void *job_thread(void *arg) { fjob_run((FJob *)arg); return NULL; }

int fjob_start(FJob *j)
{
    pthread_t *t = malloc(sizeof(pthread_t));
    j->done = 0; j->cancel = 0; j->bytes_done = j->bytes_total = 0; j->files_done = j->files_total = 0;
    j->current[0] = 0; j->error[0] = 0;
    if (!t) return -1;
    if (pthread_create(t, NULL, job_thread, j) != 0) { free(t); return -1; }
    j->thread = t;
    return 0;
}

void fjob_finish(FJob *j)
{
    if (j->thread) { pthread_join(*(pthread_t *)j->thread, NULL); free(j->thread); j->thread = NULL; }
    free(j->names); j->names = NULL; j->nnames = 0;
}
