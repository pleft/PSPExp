/* The FILE channel on a desktop: see pspexp_fs_posix.h */
#define _GNU_SOURCE
#include "pspexp_fs_posix.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#if defined(__APPLE__)
#include <sys/mount.h>
#else
#include <sys/statvfs.h>
#endif

static char g_root[900];

/* "ms0:/a/b" -> "<root>/a/b"; "ef0:/a" -> "<root>/ef0/a" */
static void local(const char *p, char *out, size_t n)
{
    const char *rest = p + 4;   /* after "ms0:" */
    while (*rest == '/') rest++;
    snprintf(out, n, "%s%s%s%s", g_root, strncmp(p, "ef0:", 4) ? "" : "/ef0", *rest ? "/" : "", rest);
}

static int err_of(int e)
{
    switch (e) {
    case ENOENT: case ENOTDIR: return PSPEXP_FSERR_NOT_FOUND;
    case EEXIST: return PSPEXP_FSERR_EXISTS;
    case ENOTEMPTY: return PSPEXP_FSERR_NOT_EMPTY;
    case ENOSPC: return PSPEXP_FSERR_FULL;
    case ENAMETOOLONG: return PSPEXP_FSERR_PATH;
    default: return PSPEXP_FSERR_IO;
    }
}

static void fill(PspExpFsEntry *e, const char *name, const struct stat *st)
{
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->size_lo = (uint32_t)st->st_size;
    e->size_hi = (uint32_t)((uint64_t)st->st_size >> 32);
    e->mtime = (uint32_t)st->st_mtime;
    e->flags = S_ISDIR(st->st_mode) ? PSPEXP_FS_DIR : 0;
}

static int fs_list(void *u, const char *path, uint32_t first, PspExpFsEntry *out, int max, int *more)
{
    char dir[1024];
    DIR *d;
    struct dirent *de;
    uint32_t i = 0;
    int n = 0;
    (void)u;
    local(path, dir, sizeof(dir));
    if (!(d = opendir(dir))) return err_of(errno);
    *more = 0;
    while ((de = readdir(d))) {
        char p[1400];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (i++ < first) continue;
        if (n == max) { *more = 1; break; }
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        if (stat(p, &st) != 0) continue;
        fill(&out[n++], de->d_name, &st);
    }
    closedir(d);
    return n;
}

static int fs_stat(void *u, const char *path, PspExpFsEntry *out)
{
    char p[1024];
    struct stat st;
    const char *name = strrchr(path, '/');
    (void)u;
    local(path, p, sizeof(p));
    if (stat(p, &st) != 0) return err_of(errno);
    fill(out, name ? name + 1 : path, &st);
    return 0;
}

static int fs_read(void *u, const char *path, uint64_t off, void *buf, uint32_t len)
{
    char p[1024];
    int fd;
    ssize_t n;
    (void)u;
    local(path, p, sizeof(p));
    if ((fd = open(p, O_RDONLY)) < 0) return err_of(errno);
    n = pread(fd, buf, len, (off_t)off);
    close(fd);
    return n < 0 ? PSPEXP_FSERR_IO : (int)n;
}

static int fs_write(void *u, const char *path, uint64_t off, const void *buf, uint32_t len, uint32_t flags)
{
    char p[1024];
    int fd;
    ssize_t n;
    (void)u;
    local(path, p, sizeof(p));
    fd = open(p, O_WRONLY | ((flags & PSPEXP_FS_CREATE) ? O_CREAT : 0) | ((flags & PSPEXP_FS_TRUNCATE) ? O_TRUNC : 0), 0644);
    if (fd < 0) return err_of(errno);
    n = pwrite(fd, buf, len, (off_t)off);
    if (n < 0) { int e = errno; close(fd); return err_of(e); }
    close(fd);
    return (int)n;
}

static int fs_mkdir(void *u, const char *path)
{
    char p[1024];
    (void)u;
    local(path, p, sizeof(p));
    return mkdir(p, 0755) == 0 ? 0 : err_of(errno);
}

static int fs_remove(void *u, const char *path)
{
    char p[1024];
    struct stat st;
    (void)u;
    local(path, p, sizeof(p));
    if (stat(p, &st) != 0) return err_of(errno);
    if (S_ISDIR(st.st_mode)) return rmdir(p) == 0 ? 0 : err_of(errno);
    return unlink(p) == 0 ? 0 : err_of(errno);
}

static int fs_rename(void *u, const char *from, const char *to)
{
    char a[1024], b[1024];
    struct stat st;
    (void)u;
    local(from, a, sizeof(a));
    local(to, b, sizeof(b));
    if (stat(b, &st) == 0) return PSPEXP_FSERR_EXISTS;
    return rename(a, b) == 0 ? 0 : err_of(errno);
}

static int fs_info(void *u, const char *path, uint64_t *fr, uint64_t *tot)
{
    char p[1024];
    (void)u;
    local(path, p, sizeof(p));
    {
#if defined(__APPLE__)
        struct statfs s;
        if (statfs(p, &s) != 0) return err_of(errno);
#else
        struct statvfs s;
        if (statvfs(p, &s) != 0) return err_of(errno);
#endif
        *fr = (uint64_t)s.f_bavail * s.f_bsize;
        *tot = (uint64_t)s.f_blocks * s.f_bsize;
    }
    return 0;
}

void pspexp_fs_posix_install(PspExpRuntimeOps *ops, const char *root)
{
    if (!root || !*root) root = getenv("PSPEXP_MS0");
    if (!root || !*root) return;
    snprintf(g_root, sizeof(g_root), "%s", root);
    mkdir(g_root, 0755);
    ops->fs_list = fs_list; ops->fs_stat = fs_stat; ops->fs_read = fs_read; ops->fs_write = fs_write;
    ops->fs_mkdir = fs_mkdir; ops->fs_remove = fs_remove; ops->fs_rename = fs_rename; ops->fs_info = fs_info;
}

/* ---- the clock ---------------------------------------------------------------- */

static int64_t g_clock_shift;   /* what CLOCK_SET moved it by */

static int clock_get(void *u, uint64_t *utc, int *tz, int *dst)
{
    time_t now = time(NULL);
    struct tm lt;
    (void)u;
    localtime_r(&now, &lt);
    *utc = (uint64_t)((int64_t)now + g_clock_shift);
    *tz = (int)(lt.tm_gmtoff / 60) - (lt.tm_isdst > 0 ? 60 : 0);
    *dst = lt.tm_isdst > 0;
    return 0;
}

static int clock_set(void *u, uint64_t utc)
{
    (void)u;
    g_clock_shift = (int64_t)utc - (int64_t)time(NULL);
    return 0;
}

void pspexp_clock_posix_install(PspExpRuntimeOps *ops) { ops->clock_get = clock_get; ops->clock_set = clock_set; }
