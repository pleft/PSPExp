/* The FILE channel on the PSP: see pspexp_fs_psp.h */
#include "pspexp_fs_psp.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

/* sceIo errors are 0x8001xxxx with an errno in the low bits */
static int err_of(int e)
{
    switch (e & 0xffff) {
    case 2: case 20: return PSPEXP_FSERR_NOT_FOUND;    /* ENOENT, ENOTDIR */
    case 17: return PSPEXP_FSERR_EXISTS;               /* EEXIST */
    case 66: case 90: return PSPEXP_FSERR_NOT_EMPTY;   /* ENOTEMPTY (BSD, newlib) */
    case 28: return PSPEXP_FSERR_FULL;                 /* ENOSPC */
    case 91: return PSPEXP_FSERR_PATH;                 /* ENAMETOOLONG */
    default: return PSPEXP_FSERR_IO;
    }
}

/* seconds since 1970 (the stick's clock is local time; so is the host's view) */
static uint32_t unix_time(const ScePspDateTime *t)
{
    int y = t->year, m = t->month, era;
    unsigned yoe, doy, doe;
    if (y < 1970 || m < 1 || m > 12) return 0;
    y -= m <= 2;
    era = y / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + t->day - 1u;
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (uint32_t)(((era * 146097 + (int)doe - 719468) * 86400) + t->hour * 3600 + t->minute * 60 + t->second);
}

static void fill(PspExpFsEntry *e, const char *name, const SceIoStat *st)
{
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->size_lo = (uint32_t)st->st_size;
    e->size_hi = (uint32_t)((uint64_t)st->st_size >> 32);
    e->mtime = unix_time(&st->sce_st_mtime);
    e->flags = FIO_S_ISDIR(st->st_mode) ? PSPEXP_FS_DIR : 0;
}

static int fs_list(void *u, const char *path, uint32_t first, PspExpFsEntry *out, int max, int *more)
{
    static SceIoDirent de;
    uint32_t i = 0;
    int n = 0, d = sceIoDopen(path);
    (void)u;
    if (d < 0) return err_of(d);
    *more = 0;
    for (;;) {
        memset(&de, 0, sizeof(de));   /* d_private must be clear, or Dread writes through it */
        if (sceIoDread(d, &de) <= 0) break;
        if (!strcmp(de.d_name, ".") || !strcmp(de.d_name, "..")) continue;
        if (i++ < first) continue;
        if (n == max) { *more = 1; break; }
        fill(&out[n++], de.d_name, &de.d_stat);
    }
    sceIoDclose(d);
    return n;
}

static int fs_stat(void *u, const char *path, PspExpFsEntry *out)
{
    SceIoStat st;
    const char *name = strrchr(path, '/');
    int rc;
    (void)u;
    memset(&st, 0, sizeof(st));
    if ((rc = sceIoGetstat(path, &st)) < 0) return err_of(rc);
    fill(out, name && name[1] ? name + 1 : path, &st);
    return 0;
}

static int fs_read(void *u, const char *path, uint64_t off, void *buf, uint32_t len)
{
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0), n;
    (void)u;
    if (fd < 0) return err_of(fd);
    if (sceIoLseek(fd, (SceOff)off, PSP_SEEK_SET) < 0) { sceIoClose(fd); return PSPEXP_FSERR_IO; }
    n = sceIoRead(fd, buf, len);
    sceIoClose(fd);
    return n < 0 ? err_of(n) : n;
}

static int fs_write(void *u, const char *path, uint64_t off, const void *buf, uint32_t len, uint32_t flags)
{
    int fd = sceIoOpen(path, PSP_O_WRONLY | ((flags & PSPEXP_FS_CREATE) ? PSP_O_CREAT : 0) | ((flags & PSPEXP_FS_TRUNCATE) ? PSP_O_TRUNC : 0), 0777), n;
    (void)u;
    if (fd < 0) return err_of(fd);
    if (sceIoLseek(fd, (SceOff)off, PSP_SEEK_SET) < 0) { sceIoClose(fd); return PSPEXP_FSERR_IO; }
    n = sceIoWrite(fd, buf, len);
    sceIoClose(fd);
    if (n >= 0 && (uint32_t)n < len) return PSPEXP_FSERR_FULL;
    return n < 0 ? err_of(n) : n;
}

static int fs_mkdir(void *u, const char *path)
{
    int rc = sceIoMkdir(path, 0777);
    (void)u;
    return rc < 0 ? err_of(rc) : 0;
}

static int fs_remove(void *u, const char *path)
{
    SceIoStat st;
    int rc;
    (void)u;
    memset(&st, 0, sizeof(st));
    if ((rc = sceIoGetstat(path, &st)) < 0) return err_of(rc);
    if (FIO_S_ISDIR(st.st_mode)) {
        int d = sceIoDopen(path);   /* some firmware removes a folder with files in it: look first */
        if (d >= 0) {
            static SceIoDirent de;
            int busy = 0;
            for (;;) {
                memset(&de, 0, sizeof(de));
                if (sceIoDread(d, &de) <= 0) break;
                if (strcmp(de.d_name, ".") && strcmp(de.d_name, "..")) { busy = 1; break; }
            }
            sceIoDclose(d);
            if (busy) return PSPEXP_FSERR_NOT_EMPTY;
        }
        rc = sceIoRmdir(path);
    } else rc = sceIoRemove(path);
    return rc < 0 ? err_of(rc) : 0;
}

static int fs_rename(void *u, const char *from, const char *to)
{
    SceIoStat st;
    int rc;
    (void)u;
    memset(&st, 0, sizeof(st));
    if (sceIoGetstat(to, &st) >= 0) return PSPEXP_FSERR_EXISTS;
    rc = sceIoRename(from, to);
    return rc < 0 ? err_of(rc) : 0;
}

static int fs_info(void *u, const char *path, uint64_t *fr, uint64_t *tot)
{
    struct { uint32_t max_clusters, free_clusters, max_sectors, sector_size, sectors_per_cluster; } inf;
    void *p = &inf;
    char dev[8];
    int rc;
    (void)u;
    memset(&inf, 0, sizeof(inf));
    snprintf(dev, sizeof(dev), "%.4s", path);   /* "ms0:" */
    if ((rc = sceIoDevctl(dev, 0x02425818, &p, sizeof(p), NULL, 0)) < 0) return err_of(rc);
    *fr = (uint64_t)inf.free_clusters * inf.sectors_per_cluster * inf.sector_size;
    *tot = (uint64_t)inf.max_clusters * inf.sectors_per_cluster * inf.sector_size;
    return 0;
}

void pspexp_fs_psp_ops(PspExpRuntimeOps *ops)
{
    ops->fs_list = fs_list; ops->fs_stat = fs_stat; ops->fs_read = fs_read; ops->fs_write = fs_write;
    ops->fs_mkdir = fs_mkdir; ops->fs_remove = fs_remove; ops->fs_rename = fs_rename; ops->fs_info = fs_info;
}
