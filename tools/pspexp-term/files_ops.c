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

/* ---- the two kinds of path ------------------------------------------------------ */

static const FRemote *g_rem;

void fops_set_remote(const FRemote *r) { g_rem = r; }

int fops_is_remote(const char *p) { return !strncmp(p, "ms0:", 4) || !strncmp(p, "ef0:", 4); }

int fops_is_root(const char *p) { return !strcmp(p, "/") || (fops_is_remote(p) && !strcmp(p + 4, "/")); }

const char *fops_strerror(int e)
{
    if (e == ENOTCONN) return "the PSP did not answer";
    if (e == ENOTSUP) return "the PSP runtime does not offer this";
    return strerror(e);
}

/* a remote call's result: >= 0, or -1 with errno */
static int rem(int rc) { if (rc < 0) { errno = -rc; return -1; } return rc; }
static int no_remote(void) { errno = ENOTCONN; return -1; }

/* lstat's view (a link is itself); 0 or -1 with errno */
static int v_stat(const char *p, FEntry *e)
{
    struct stat st;
    memset(e, 0, sizeof(*e));
    if (fops_is_remote(p)) return g_rem ? rem(g_rem->stat(p, e)) : no_remote();
    if (lstat(p, &st) < 0) return -1;
    e->is_link = S_ISLNK(st.st_mode);
    e->is_dir = S_ISDIR(st.st_mode);
    e->size = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
    e->mtime = st.st_mtime;
    e->mode = st.st_mode;
    return 0;
}

/* a folder's entries, unsorted, no "." or ".."; follow: a link shows what it points to */
static int v_entries(const char *dir, FList *out, int follow)
{
    DIR *d;
    struct dirent *de;
    out->n = 0;
    if (fops_is_remote(dir)) return g_rem ? rem(g_rem->list(dir, out)) : no_remote();
    if (!(d = opendir(dir))) return -1;
    while ((de = readdir(d))) {
        FEntry *e;
        struct stat st, lst;
        char p[FOPS_PATH];
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
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
            if (follow && e->is_link && stat(p, &st) == 0) lst = st;
            e->is_dir = S_ISDIR(lst.st_mode);
            e->size = (uint64_t)lst.st_size;
            e->mtime = lst.st_mtime;
            e->mode = lst.st_mode;
        }
        out->n++;
    }
    closedir(d);
    return 0;
}

static int v_mkdir(const char *p, mode_t mode)
{
    if (fops_is_remote(p)) return g_rem ? rem(g_rem->mkdir(p)) : no_remote();
    return mkdir(p, mode);
}

static int v_remove(const char *p, int is_dir)
{
    if (fops_is_remote(p)) return g_rem ? rem(g_rem->remove(p)) : no_remote();
    return is_dir ? rmdir(p) : unlink(p);
}

/* on one drive; EXDEV between them */
static int v_rename(const char *a, const char *b)
{
    int ra = fops_is_remote(a), rb = fops_is_remote(b);
    if (ra != rb || (ra && strncmp(a, b, 4))) { errno = EXDEV; return -1; }
    if (ra) return g_rem ? rem(g_rem->rename(a, b)) : no_remote();
    return rename(a, b);
}

/* ---- listings --- ---------------------------------------------------------------- */

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
    FList raw = { NULL, 0, 0 };
    int i;
    out->n = 0;
    if (v_entries(dir, &raw, 1) < 0) { int e = errno; fops_free(&raw); errno = e; return -1; }
    if (out->cap < raw.n + 1) {
        FEntry *e = realloc(out->e, sizeof(FEntry) * (size_t)(raw.n + 16));
        if (!e) { fops_free(&raw); errno = ENOMEM; return -1; }
        out->e = e; out->cap = raw.n + 16;
    }
    if (!fops_is_root(dir)) {
        memset(&out->e[0], 0, sizeof(FEntry));
        strcpy(out->e[0].name, "..");
        out->e[0].is_dir = out->e[0].is_up = 1;
        out->n = 1;
    }
    for (i = 0; i < raw.n; i++)
        if (show_hidden || raw.e[i].name[0] != '.') out->e[out->n++] = raw.e[i];
    fops_free(&raw);
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
    if (s == path || (fops_is_remote(path) && s == path + 4)) s[1] = 0;   /* "/x" -> "/", "ms0:/x" -> "ms0:/" */
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
    if (v_mkdir(path, 0755) == 0) return 0;
    snprintf(err, en, "%s", fops_strerror(errno));
    return -1;
}

int fops_rename(const char *from, const char *to, char *err, size_t en)
{
    FEntry e;
    if (v_stat(to, &e) == 0) { snprintf(err, en, "that name is taken"); return -1; }
    if (v_rename(from, to) == 0) return 0;
    snprintf(err, en, "%s", fops_strerror(errno));
    return -1;
}

int fops_conflicts(const char *dst, char (*names)[256], int n, char *first, size_t fn)
{
    int i, c = 0;
    FEntry e;
    char p[FOPS_PATH];
    if (first && fn) first[0] = 0;
    for (i = 0; i < n; i++) {
        fops_join(p, sizeof(p), dst, names[i]);
        if (v_stat(p, &e) == 0) { if (!c && first) snprintf(first, fn, "%s", names[i]); c++; }
    }
    return c;
}

/* ---- jobs -------------------------------------------------------------------- */

static void job_err(FJob *j, const char *what, const char *path)
{
    if (!j->error[0]) snprintf(j->error, sizeof(j->error), "%s %s: %s", what, path, fops_strerror(errno));
}

static const char *base_of(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

/* the size and number of files below path (a file: itself) */
static void measure(FJob *j, const char *path)
{
    FEntry st;
    if (j->cancel || v_stat(path, &st) < 0) return;
    if (st.is_dir) {
        FList l = { NULL, 0, 0 };
        int i;
        if (v_entries(path, &l, 0) == 0)
            for (i = 0; i < l.n && !j->cancel; i++) {
                char p[FOPS_PATH];
                fops_join(p, sizeof(p), path, l.e[i].name);
                measure(j, p);
            }
        fops_free(&l);
    } else {
        j->bytes_total += st.size;
        j->files_total++;
    }
}

static int remove_tree(FJob *j, const char *path)
{
    FEntry st;
    if (j->cancel) return -1;
    if (v_stat(path, &st) < 0) { job_err(j, "cannot read", path); return -1; }
    if (st.is_dir) {
        FList l = { NULL, 0, 0 };   /* all names first: a listing read in pages shifts while it empties */
        int rc = 0, i;
        if (v_entries(path, &l, 0) < 0) { job_err(j, "cannot open", path); fops_free(&l); return -1; }
        for (i = 0; i < l.n && !rc; i++) {
            char p[FOPS_PATH];
            fops_join(p, sizeof(p), path, l.e[i].name);
            rc = remove_tree(j, p);
        }
        fops_free(&l);
        if (rc) return rc;
        if (v_remove(path, 1) < 0) { job_err(j, "cannot delete", path); return -1; }
        return 0;
    }
    snprintf(j->current, sizeof(j->current), "%s", base_of(path));
    if (v_remove(path, 0) < 0) { job_err(j, "cannot delete", path); return -1; }
    j->files_done++;
    j->bytes_done += st.size;
    return 0;
}

/* one file, from and to either kind of drive */
static int copy_file(FJob *j, const char *from, const char *to, mode_t mode)
{
    int rin = fops_is_remote(from), rout = fops_is_remote(to), in = -1, out = -1, rc = 0;
    size_t chunk = (rin || rout) && g_rem ? g_rem->chunk : 256 * 1024;
    uint64_t off = 0;
    char *buf;
    ssize_t n = 0;
    snprintf(j->current, sizeof(j->current), "%s", base_of(from));
    if ((rin || rout) && !g_rem) { no_remote(); job_err(j, "cannot copy", from); return -1; }
    if (!(buf = malloc(chunk))) { snprintf(j->error, sizeof(j->error), "out of memory"); return -1; }
    if (!rin && (in = open(from, O_RDONLY)) < 0) { job_err(j, "cannot read", from); free(buf); return -1; }
    if (!rout && (out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode & 07777)) < 0) { job_err(j, "cannot write", to); if (in >= 0) close(in); free(buf); return -1; }
    for (;;) {
        if (j->cancel) { rc = -1; break; }
        n = rin ? rem(g_rem->read(from, off, buf, chunk)) : read(in, buf, chunk);
        if (n < 0) { if (!rin && errno == EINTR) continue; job_err(j, "cannot read", from); rc = -1; break; }
        if (n == 0 && (off || !rout)) break;   /* an empty file still makes one (empty) write */
        if (rout) {
            if (rem(g_rem->write(to, off, buf, (size_t)n, off == 0)) < 0) { job_err(j, "cannot write", to); rc = -1; break; }
        } else {
            char *p = buf;
            ssize_t left = n;
            while (left > 0) {
                ssize_t w = write(out, p, (size_t)left);
                if (w < 0) { if (errno == EINTR) continue; job_err(j, "cannot write", to); rc = -1; break; }
                p += w; left -= w;
            }
            if (rc) break;
        }
        off += (uint64_t)n; j->bytes_done += (uint64_t)n;
        if (n == 0 || (rin && (size_t)n < chunk)) break;   /* the PSP's last piece: no need to ask again */
    }
    if (out >= 0 && close(out) < 0 && !rc) { job_err(j, "cannot write", to); rc = -1; }
    if (in >= 0) close(in);
    free(buf);
    if (rc) v_remove(to, 0);   /* no half files */
    else j->files_done++;
    return rc;
}

static int copy_tree(FJob *j, const char *from, const char *to)
{
    FEntry st;
    if (j->cancel) return -1;
    if (v_stat(from, &st) < 0) { job_err(j, "cannot read", from); return -1; }
    if (st.is_link) {
        char target[FOPS_PATH];
        ssize_t l;
        if (fops_is_remote(to)) {   /* the PSP has no links: what it points to */
            struct stat s2;
            if (stat(from, &s2) < 0) { j->files_done++; return 0; }   /* dangling: skipped */
            st.is_dir = S_ISDIR(s2.st_mode); st.size = (uint64_t)s2.st_size; st.mode = s2.st_mode; st.is_link = 0;
            if (!st.is_dir && !S_ISREG(s2.st_mode)) { j->files_done++; return 0; }
        } else {
            if ((l = readlink(from, target, sizeof(target) - 1)) < 0) { job_err(j, "cannot read", from); return -1; }
            target[l] = 0;
            unlink(to);
            if (symlink(target, to) < 0) { job_err(j, "cannot write", to); return -1; }
            j->files_done++;
            return 0;
        }
    }
    if (st.is_dir) {
        FList l = { NULL, 0, 0 };
        int rc = 0, i;
        if (v_mkdir(to, st.mode ? (st.mode & 07777) : 0755) < 0 && errno != EEXIST) { job_err(j, "cannot create", to); return -1; }
        if (v_entries(from, &l, 0) < 0) { job_err(j, "cannot open", from); fops_free(&l); return -1; }
        for (i = 0; i < l.n && !rc; i++) {
            char a[FOPS_PATH], b[FOPS_PATH];
            fops_join(a, sizeof(a), from, l.e[i].name);
            fops_join(b, sizeof(b), to, l.e[i].name);
            rc = copy_tree(j, a, b);
        }
        fops_free(&l);
        return rc;
    }
    if (!fops_is_remote(from) && !S_ISREG(st.mode)) { j->files_done++; return 0; }   /* devices, pipes: skipped */
    return copy_file(j, from, to, st.mode ? st.mode : 0644);
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
            FEntry st;
            if (v_stat(b, &st) == 0) remove_tree(j, b);   /* the view asked before replacing */
            if (v_rename(a, b) == 0) { j->files_done++; continue; }
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
