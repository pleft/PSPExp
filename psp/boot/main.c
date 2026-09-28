/*
 * PSPExp boot: loads the kernel USB driver and the user-mode runtime, then
 * gets out of the way. Same shape as PSPLink's bootstrap: a user module whose
 * main thread has attribute 0, which custom firmware runs in kernel mode.
 *
 * The runtime lives in two slots next to the EBOOT (docs/Package_Manager.md
 * §6.1): slot-a/ and slot-b/, each a complete set (pspexp_runtime.prx,
 * pspexp_usb.prx) with a VERSION file listing every file's size and SHA-256.
 *   - A slot is valid only when VERSION is there, well formed, and every file
 *     it lists is present with that size and checksum. The loader checks this
 *     in full before booting any slot: the chosen one, a fallback, a forced one.
 *   - `active` names the slot to boot and counts boot attempts (tries); the
 *     runtime sets tries back to 0 once it has a working connection.
 *   - `active.new`, when present and naming a valid slot, is the current choice
 *     (a console's update ends by writing it: the one switch point); the loader
 *     makes it `active`.
 *   - tries reaching 3, an invalid chosen slot, or modules that fail to load:
 *     the other slot, when valid, recorded in `fallback`. Holding L at start:
 *     the other slot, by hand.
 *   - No slot folders at all: the runtime next to the EBOOT, as before slots.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <stdio.h>
#include <string.h>
#include "../../protocol/pspexp_sha256.h"

PSP_MODULE_INFO("PSPExpBoot", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(0);

#define USB_MODULE     "pspexp_usb.prx"
#define RUNTIME_MODULE "pspexp_runtime.prx"
#define MAX_TRIES      3

static char g_dir[384];   /* the EBOOT's folder, with its trailing / */

static int load_start(const char *dir, const char *name, SceSize args, void *argp)
{
    char path[512];
    SceUID mod;
    int status = 0;
    snprintf(path, sizeof(path), "%s%s", dir, name);
    mod = sceKernelLoadModule(path, 0, NULL);
    if (mod < 0) { pspDebugScreenPrintf("load %s: 0x%08X\n", name, mod); return -1; }
    mod = sceKernelStartModule(mod, args, argp, &status, NULL);
    if (mod < 0) { pspDebugScreenPrintf("start %s: 0x%08X\n", name, mod); return -1; }
    return 0;
}

static int read_small(const char *name, char *buf, int max)
{
    char path[512];
    SceUID fd;
    int n;
    snprintf(path, sizeof(path), "%s%s", g_dir, name);
    if ((fd = sceIoOpen(path, PSP_O_RDONLY, 0)) < 0) return -1;
    n = sceIoRead(fd, buf, max - 1);
    sceIoClose(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}

static void write_small(const char *name, const char *text)
{
    char path[512];
    SceUID fd;
    snprintf(path, sizeof(path), "%s%s", g_dir, name);
    if ((fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777)) < 0) return;
    sceIoWrite(fd, text, strlen(text));
    sceIoClose(fd);
}

static int exists(const char *name)
{
    char path[512];
    SceIoStat st;
    snprintf(path, sizeof(path), "%s%s", g_dir, name);
    return sceIoGetstat(path, &st) >= 0;
}

/* "slot=a\ntries=1\n" -> 'a' and 1; 0 when not readable */
static char parse_active(const char *text, int *tries)
{
    const char *s = strstr(text, "slot="), *t = strstr(text, "tries=");
    *tries = 0;
    if (t) { t += 6; while (*t >= '0' && *t <= '9') *tries = *tries * 10 + (*t++ - '0'); }
    if (s && (s[5] == 'a' || s[5] == 'b')) return s[5];
    return 0;
}

/* the whole set of slot s checks against its VERSION */
static int slot_valid(char s, char *version, int vmax)
{
    static char text[2048];
    static uint8_t buf[16384];
    char vname[32], *line, *next;
    int files = 0, runtime = 0, end = 0;
    snprintf(vname, sizeof(vname), "slot-%c/VERSION", s);
    if (read_small(vname, text, sizeof(text)) <= 0) return 0;
    if (version) version[0] = 0;
    for (line = text; line && *line; line = next) {
        char name[64], hex[65], got[65], path[512];
        unsigned size = 0;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, "version=", 8) && version) { snprintf(version, vmax, "%s", line + 8); continue; }
        if (!strcmp(line, "end")) { end = 1; break; }   /* the last line: a VERSION cut short never has it */
        if (strncmp(line, "file=", 5)) continue;
        if (sscanf(line, "file=%63s size=%u sha256=%64s", name, &size, hex) != 3 || strchr(name, '/')) return 0;
        {   /* the file: present, that size, that checksum */
            PspExpSha256 h;
            uint8_t d[32];
            SceUID fd;
            unsigned total = 0;
            int n;
            snprintf(path, sizeof(path), "%sslot-%c/%s", g_dir, s, name);
            if ((fd = sceIoOpen(path, PSP_O_RDONLY, 0)) < 0) return 0;
            pspexp_sha256_init(&h);
            while ((n = sceIoRead(fd, buf, sizeof(buf))) > 0) { pspexp_sha256_update(&h, buf, (size_t)n); total += (unsigned)n; }
            sceIoClose(fd);
            if (n < 0 || total != size) return 0;
            pspexp_sha256_final(&h, d);
            pspexp_sha256_hex(d, got);
            if (strcmp(got, hex)) return 0;
        }
        files++;
        if (!strcmp(name, RUNTIME_MODULE)) runtime = 1;
    }
    return files > 0 && runtime && end;
}

static int boot_main(SceSize args, void *argp)
{
    char text[256], version[64], slotdir[400], *slash;
    char slot, other;
    int tries = 0, valid, forced = 0;
    SceCtrlData pad;

    pspDebugScreenInit();
    pspDebugScreenPrintf("PSPExp boot\n");
    snprintf(g_dir, sizeof(g_dir), "%s", (const char *)argp);
    slash = strrchr(g_dir, '/');
    if (!slash) { pspDebugScreenPrintf("bad path %s\n", g_dir); goto fail; }
    slash[1] = 0;

    if (!exists("slot-a/VERSION") && !exists("slot-b/VERSION") && !exists("active") && !exists("active.new")) {
        snprintf(slotdir, sizeof(slotdir), "%s", g_dir);   /* before slots: next to the EBOOT */
        goto load;
    }
    /* the choice: active.new (a finished update) over active */
    slot = 0;
    if (read_small("active.new", text, sizeof(text)) > 0 && (slot = parse_active(text, &tries)) && slot_valid(slot, NULL, 0)) {
        char a[512], b[512];
        snprintf(a, sizeof(a), "%sactive", g_dir);
        snprintf(b, sizeof(b), "%sactive.new", g_dir);
        sceIoRemove(a);
        sceIoRename(b, a);
        tries = 0;
    } else {
        if (exists("active.new")) {   /* cut short, or naming a slot that does not check: never the choice */
            char b[512];
            snprintf(b, sizeof(b), "%sactive.new", g_dir);
            sceIoRemove(b);
            slot = 0;
        }
        if (read_small("active", text, sizeof(text)) > 0) slot = parse_active(text, &tries);
    }
    if (!slot) { slot = slot_valid('a', NULL, 0) ? 'a' : 'b'; tries = 0; }
    other = slot == 'a' ? 'b' : 'a';
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0 && (pad.Buttons & PSP_CTRL_LTRIGGER)) {
        pspDebugScreenPrintf("L held: the other slot\n");
        slot = other; other = slot == 'a' ? 'b' : 'a'; tries = 0; forced = 1;
    }
    valid = slot_valid(slot, version, sizeof(version));
    if (!valid || tries >= MAX_TRIES) {
        char why[160];
        snprintf(why, sizeof(why), "from=%c to=%c reason=%s\n", slot, other, !valid ? "not valid" : "did not connect 3 times");
        if (!slot_valid(other, version, sizeof(version))) {
            pspDebugScreenPrintf("\nNo valid PSPExp runtime (slot %c %s, slot %c not valid).\n"
                                 "Put the PSP in USB mode (Settings > USB Connection) and copy\n"
                                 "a known-good PSPExp folder to ms0:/PSP/GAME/PSPExp/\n", slot, valid ? "failed" : "not valid", other);
            sceKernelDelayThread(8000000);
            goto fail;
        }
        pspDebugScreenPrintf("slot %c: %s; slot %c instead\n", slot, !valid ? "not valid" : "did not connect", other);
        write_small("fallback", why);
        slot = other; tries = 0;
    }
    snprintf(text, sizeof(text), "slot=%c\ntries=%d\n", slot, tries + 1);
    write_small("active", text);
    pspDebugScreenPrintf("slot %c%s: %s\n", slot, forced ? " (forced)" : "", version[0] ? version : "?");
    snprintf(slotdir, sizeof(slotdir), "%sslot-%c/", g_dir, slot);
load:
    if (load_start(slotdir, USB_MODULE, 0, NULL) < 0) {
        pspDebugScreenPrintf("USB driver not available (emulator?), continuing with TCP only\n");
        sceKernelDelayThread(500000);
    }
    /* the runtime gets the EBOOT's path, as always: its log and screenshots stay next to the EBOOT */
    if (load_start(slotdir, RUNTIME_MODULE, args, argp) < 0) goto fail;
    sceKernelSelfStopUnloadModule(1, 0, NULL);
    return 0;
fail:
    sceKernelDelayThread(3000000);
    sceKernelExitGame();
    return 0;
}

int module_start(SceSize args, void *argp)
{
    SceUID thid = sceKernelCreateThread("PSPExpBoot", boot_main, 32, 0x10000, 0, NULL);
    if (thid < 0) return 1;
    sceKernelStartThread(thid, args, argp);
    return 0;
}

void _exit(int status) { sceKernelExitGame(); for (;;) {} }
