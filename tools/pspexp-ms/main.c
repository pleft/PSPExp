/*
 * pspexp-ms: the PSP's memory stick from a shell, over the PSPExp link
 * (the runtime's FILE channel). For scripts and tests; the console's file
 * manager offers the same as a drive.
 *
 *   pspexp-ms [--tcp HOST[:PORT] | --usb] COMMAND ...
 *     ls PATH              list a folder ("ms0:/PSP/GAME")
 *     get PATH [FILE]      copy a file from the PSP (to FILE, or its name here)
 *     put FILE PATH        copy a file to the PSP (PATH: the file, or a folder ending in /)
 *     mkdir PATH | rm PATH | mv FROM TO | df [DEVICE]
 */
#include "../../host/libpspexp/pspexp.h"
#include "../../host/transport-tcp/pspexp_tcp.h"
#ifdef PSPEXP_HAVE_USB
#include "../../host/transport-usb/pspexp_usb.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *err_name(int e)
{
    switch (e) {
    case PSPEXP_FSERR_NOT_FOUND: return "not found";
    case PSPEXP_FSERR_EXISTS: return "already exists";
    case PSPEXP_FSERR_NOT_EMPTY: return "folder not empty";
    case PSPEXP_FSERR_PATH: return "bad path";
    case PSPEXP_FSERR_FULL: return "memory stick full";
    case PSPEXP_FSERR_UNSUPPORTED: return "the PSP runtime does not offer files (update it)";
    case PSPEXP_FSERR_LINK: return "no reply from the PSP";
    default: return "I/O error";
    }
}

static int fail(const char *what, int e) { fprintf(stderr, "pspexp-ms: %s: %s\n", what, err_name(e)); return 1; }

static void quiet(void *u, const char *m) { (void)u; (void)m; }

static int usage(void)
{
    fprintf(stderr, "usage: pspexp-ms [--tcp HOST[:PORT] | --usb] ls PATH | get PATH [FILE] | put FILE PATH | mkdir PATH | rm PATH | mv FROM TO | df [DEVICE]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *tcp = NULL, *cmd;
    PspExpTransport *t = NULL;
    PspExpContext *ctx;
    char err[256] = "";
    int a = 1, rc = 0;
    static uint8_t buf[PSPEXP_FS_CHUNK];

    if (a < argc && !strcmp(argv[a], "--tcp") && a + 1 < argc) { tcp = argv[a + 1]; a += 2; }
    else if (a < argc && !strcmp(argv[a], "--usb")) a++;
    if (a >= argc) return usage();
    cmd = argv[a++];

    if (tcp) {
        PspExpTcpOptions o;
        static char host[256];
        char *c;
        pspexp_tcp_options_default(&o);
        snprintf(host, sizeof(host), "%s", tcp);
        if ((c = strrchr(host, ':'))) { *c = 0; o.port = (uint16_t)atoi(c + 1); }
        o.host = host;
        t = pspexp_tcp_open(&o, err, sizeof(err));
    } else {
#ifdef PSPEXP_HAVE_USB
        PspExpUsbOptions o;
        pspexp_usb_options_default(&o);
        t = pspexp_usb_open(&o, err, sizeof(err));
#else
        snprintf(err, sizeof(err), "built without USB: use --tcp");
#endif
    }
    if (!t) { fprintf(stderr, "pspexp-ms: %s\n", err); return 1; }
    ctx = pspexp_create(t, NULL);
    pspexp_set_log(ctx, quiet, NULL);
    if (pspexp_connect(ctx, 5000) < 0) { fprintf(stderr, "pspexp-ms: no PSPExp runtime answered\n"); return 1; }
    if (!pspexp_fs_supported(ctx)) return fail("files", PSPEXP_FSERR_UNSUPPORTED);

    if (!strcmp(cmd, "ls") && a < argc) {
        PspExpFsEntry e[PSPEXP_FS_LIST_MAX];
        uint32_t first = 0;
        int more = 1, n, i;
        while (more) {
            if ((n = pspexp_fs_list(ctx, argv[a], first, e, PSPEXP_FS_LIST_MAX, &more)) < 0) return fail(argv[a], n);
            for (i = 0; i < n; i++) {
                time_t tm = (time_t)e[i].mtime;
                char when[32];
                strftime(when, sizeof(when), "%Y-%m-%d %H:%M", gmtime(&tm));
                if (e[i].flags & PSPEXP_FS_DIR) printf("%12s  %s  %s/\n", "", when, e[i].name);
                else printf("%12llu  %s  %s\n", (unsigned long long)e[i].size_lo | ((unsigned long long)e[i].size_hi << 32), when, e[i].name);
            }
            first += (uint32_t)n;
        }
    } else if (!strcmp(cmd, "get") && a < argc) {
        const char *src = argv[a], *base = strrchr(src, '/');
        const char *dst = a + 1 < argc ? argv[a + 1] : base ? base + 1 : src;
        FILE *f = fopen(dst, "wb");
        uint64_t off = 0;
        int n;
        if (!f) { perror(dst); return 1; }
        while ((n = pspexp_fs_read(ctx, src, off, buf, sizeof(buf))) > 0) { fwrite(buf, 1, (size_t)n, f); off += (uint64_t)n; }
        fclose(f);
        if (n < 0) return fail(src, n);
        printf("%s: %llu bytes\n", dst, (unsigned long long)off);
    } else if (!strcmp(cmd, "put") && a + 1 < argc) {
        const char *src = argv[a], *base = strrchr(src, '/');
        char dst[PSPEXP_FS_PATH];
        FILE *f = fopen(src, "rb");
        uint64_t off = 0;
        size_t n;
        int w = 0;
        if (!f) { perror(src); return 1; }
        snprintf(dst, sizeof(dst), "%s%s", argv[a + 1], argv[a + 1][strlen(argv[a + 1]) - 1] == '/' ? (base ? base + 1 : src) : "");
        do {
            n = fread(buf, 1, sizeof(buf), f);
            if ((w = pspexp_fs_write(ctx, dst, off, buf, (uint32_t)n, off ? 0 : PSPEXP_FS_CREATE | PSPEXP_FS_TRUNCATE)) < 0) break;
            off += n;
        } while (n == sizeof(buf));
        fclose(f);
        if (w < 0) return fail(dst, w);
        printf("%s: %llu bytes\n", dst, (unsigned long long)off);
    } else if (!strcmp(cmd, "mkdir") && a < argc) {
        if ((rc = pspexp_fs_mkdir(ctx, argv[a])) < 0) return fail(argv[a], rc);
    } else if (!strcmp(cmd, "rm") && a < argc) {
        if ((rc = pspexp_fs_remove(ctx, argv[a])) < 0) return fail(argv[a], rc);
    } else if (!strcmp(cmd, "mv") && a + 1 < argc) {
        if ((rc = pspexp_fs_rename(ctx, argv[a], argv[a + 1])) < 0) return fail(argv[a], rc);
    } else if (!strcmp(cmd, "df")) {
        uint64_t fr, tot;
        const char *dev = a < argc ? argv[a] : "ms0:/";
        if ((rc = pspexp_fs_info(ctx, dev, &fr, &tot)) < 0) return fail(dev, rc);
        printf("%s  %.1f MB free of %.1f MB\n", dev, fr / 1048576.0, tot / 1048576.0);
    } else return usage();
    pspexp_destroy(ctx);
    return 0;
}
