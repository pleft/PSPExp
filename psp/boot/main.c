/*
 * PSPExp boot: loads the kernel USB driver and the user-mode runtime, then
 * gets out of the way. Same shape as PSPLink's bootstrap: a user module whose
 * main thread has attribute 0, which custom firmware runs in kernel mode.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <string.h>

PSP_MODULE_INFO("PSPExpBoot", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

#define USB_MODULE     "pspexp_usb.prx"
#define RUNTIME_MODULE "pspexp_runtime.prx"

static int load_start(const char *dir, const char *name, SceSize args, void *argp)
{
    char path[512];
    SceUID mod;
    int status = 0;
    strcpy(path, dir);
    strcat(path, name);
    mod = sceKernelLoadModule(path, 0, NULL);
    if (mod < 0) { pspDebugScreenPrintf("load %s: 0x%08X\n", name, mod); return -1; }
    mod = sceKernelStartModule(mod, args, argp, &status, NULL);
    if (mod < 0) { pspDebugScreenPrintf("start %s: 0x%08X\n", name, mod); return -1; }
    return 0;
}

static int boot_main(SceSize args, void *argp)
{
    char dir[512];
    char *slash;

    pspDebugScreenInit();
    pspDebugScreenPrintf("PSPExp boot\n");
    strcpy(dir, (const char *)argp);
    slash = strrchr(dir, '/');
    if (!slash) { pspDebugScreenPrintf("bad path %s\n", dir); goto fail; }
    slash[1] = 0;
    if (load_start(dir, USB_MODULE, 0, NULL) < 0) goto fail;
    if (load_start(dir, RUNTIME_MODULE, args, argp) < 0) goto fail;
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
