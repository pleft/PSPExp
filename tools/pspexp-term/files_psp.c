/* The PSP's memory stick as a file-manager drive: see files_psp.h */
#include "files_psp.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static PspExpContext *g_ctx;
static pthread_t g_owner;
static int g_have_owner;

enum { R_LIST, R_STAT, R_READ, R_WRITE, R_MKDIR, R_REMOVE, R_RENAME, R_INFO };
typedef struct Req {
    int op;
    const char *a, *b;
    uint64_t off, v1, v2;
    void *buf; const void *cbuf; size_t len; int first;
    FList *list; FEntry *ent;
    int rc, done;
} Req;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static Req *g_pending;

static int err_of(int st)
{
    switch (st) {
    case PSPEXP_FSERR_NOT_FOUND: return -ENOENT;
    case PSPEXP_FSERR_EXISTS: return -EEXIST;
    case PSPEXP_FSERR_NOT_EMPTY: return -ENOTEMPTY;
    case PSPEXP_FSERR_PATH: return -ENAMETOOLONG;
    case PSPEXP_FSERR_FULL: return -ENOSPC;
    case PSPEXP_FSERR_UNSUPPORTED: return -ENOTSUP;
    case PSPEXP_FSERR_LINK: return -ENOTCONN;
    default: return -EIO;
    }
}

/* the PSP keeps local time; as seconds since 1970 read as UTC. Here: the real instant */
static time_t psp_time(uint32_t t)
{
    time_t s = (time_t)t;
    struct tm tmv;
    if (!t) return 0;
    gmtime_r(&s, &tmv);
    tmv.tm_isdst = -1;
    return mktime(&tmv);
}

static void to_entry(FEntry *e, const PspExpFsEntry *f)
{
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", f->name);
    e->is_dir = (f->flags & PSPEXP_FS_DIR) != 0;
    e->size = e->is_dir ? 0 : (uint64_t)f->size_lo | ((uint64_t)f->size_hi << 32);
    e->mtime = psp_time(f->mtime);
    e->mode = 0;   /* the stick has no permissions */
}

static int run(Req *r)
{
    PspExpContext *c = g_ctx;
    int st;
    if (!c || !pspexp_fs_supported(c)) return c && pspexp_link_state(c) == PSPEXP_LINK_CONNECTED ? -ENOTSUP : -ENOTCONN;
    switch (r->op) {
    case R_LIST: {
        static PspExpFsEntry page[PSPEXP_FS_LIST_MAX];
        uint32_t first = 0;
        int more = 1, i;
        r->list->n = 0;
        while (more) {
            if ((st = pspexp_fs_list(c, r->a, first, page, PSPEXP_FS_LIST_MAX, &more)) < 0) return err_of(st);
            if (r->list->n + st > r->list->cap) {
                int nc = r->list->n + st + 64;
                FEntry *ne = realloc(r->list->e, sizeof(FEntry) * (size_t)nc);
                if (!ne) return -ENOMEM;
                r->list->e = ne; r->list->cap = nc;
            }
            for (i = 0; i < st; i++) to_entry(&r->list->e[r->list->n++], &page[i]);
            first += (uint32_t)st;
            if (!st) break;
        }
        return 0;
    }
    case R_STAT: {
        PspExpFsEntry f;
        if ((st = pspexp_fs_stat(c, r->a, &f)) < 0) return err_of(st);
        to_entry(r->ent, &f);
        return 0;
    }
    case R_READ:
        st = pspexp_fs_read(c, r->a, r->off, r->buf, (uint32_t)(r->len > PSPEXP_FS_CHUNK ? PSPEXP_FS_CHUNK : r->len));
        return st < 0 ? err_of(st) : st;
    case R_WRITE:
        if (r->len > PSPEXP_FS_CHUNK) return -EINVAL;
        st = pspexp_fs_write(c, r->a, r->off, r->cbuf, (uint32_t)r->len, r->first ? PSPEXP_FS_CREATE | PSPEXP_FS_TRUNCATE : 0);
        return st < 0 ? err_of(st) : st;
    case R_MKDIR:  st = pspexp_fs_mkdir(c, r->a); return st < 0 ? err_of(st) : 0;
    case R_REMOVE: st = pspexp_fs_remove(c, r->a); return st < 0 ? err_of(st) : 0;
    case R_RENAME: st = pspexp_fs_rename(c, r->a, r->b); return st < 0 ? err_of(st) : 0;
    case R_INFO:   st = pspexp_fs_info(c, r->a, &r->v1, &r->v2); return st < 0 ? err_of(st) : 0;
    }
    return -EINVAL;
}

/* here when on the link's thread, else through it */
static int call(Req *r)
{
    if (!g_have_owner || pthread_equal(pthread_self(), g_owner)) return run(r);
    pthread_mutex_lock(&g_mu);
    while (g_pending) pthread_cond_wait(&g_cv, &g_mu);
    r->done = 0;
    g_pending = r;
    pthread_cond_broadcast(&g_cv);
    while (!r->done) pthread_cond_wait(&g_cv, &g_mu);
    pthread_mutex_unlock(&g_mu);
    return r->rc;
}

void files_psp_service(PspExpContext *ctx, uint32_t budget_us)
{
    uint64_t end = pspexp_time_us() + budget_us;
    int served = 0;
    g_ctx = ctx;
    for (;;) {
        Req *r;
        pthread_mutex_lock(&g_mu);
        if (!g_pending && served) {   /* a job asks again right after an answer: wait for it a moment */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 2000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_cv, &g_mu, &ts);
        }
        r = g_pending;
        pthread_mutex_unlock(&g_mu);
        if (!r) return;
        r->rc = run(r);
        pthread_mutex_lock(&g_mu);
        r->done = 1;
        g_pending = NULL;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        served++;
        if (pspexp_time_us() >= end) return;
    }
}

static int f_list(const char *dir, FList *out) { Req r; memset(&r, 0, sizeof(r)); r.op = R_LIST; r.a = dir; r.list = out; return call(&r); }
static int f_stat(const char *p, FEntry *out) { Req r; memset(&r, 0, sizeof(r)); r.op = R_STAT; r.a = p; r.ent = out; return call(&r); }
static int f_read(const char *p, uint64_t off, void *buf, size_t len) { Req r; memset(&r, 0, sizeof(r)); r.op = R_READ; r.a = p; r.off = off; r.buf = buf; r.len = len; return call(&r); }
static int f_write(const char *p, uint64_t off, const void *buf, size_t len, int first) { Req r; memset(&r, 0, sizeof(r)); r.op = R_WRITE; r.a = p; r.off = off; r.cbuf = buf; r.len = len; r.first = first; return call(&r); }
static int f_mkdir(const char *p) { Req r; memset(&r, 0, sizeof(r)); r.op = R_MKDIR; r.a = p; return call(&r); }
static int f_remove(const char *p) { Req r; memset(&r, 0, sizeof(r)); r.op = R_REMOVE; r.a = p; return call(&r); }
static int f_rename(const char *a, const char *b) { Req r; memset(&r, 0, sizeof(r)); r.op = R_RENAME; r.a = a; r.b = b; return call(&r); }

static const FRemote REMOTE = { f_list, f_stat, f_read, f_write, f_mkdir, f_remove, f_rename, PSPEXP_FS_CHUNK };

void files_psp_init(PspExpContext *ctx)
{
    g_ctx = ctx;
    g_owner = pthread_self();
    g_have_owner = 1;
    fops_set_remote(&REMOTE);
}

int files_psp_available(void) { return g_ctx && pspexp_fs_supported(g_ctx); }

int files_psp_space(const char *path, uint64_t *fr, uint64_t *tot)
{
    Req r;
    memset(&r, 0, sizeof(r));
    r.op = R_INFO; r.a = path;
    if (call(&r) < 0) return -1;
    *fr = r.v1; *tot = r.v2;
    return 0;
}
