/*
 * What the system monitor shows, read from the board: Linux's /proc and /sys
 * (the boards), a smaller set on macOS (a Mac as the console). Rates and CPU
 * shares come from the change since the previous sysinfo_update.
 */
#ifndef PSPEXP_TERM_SYSINFO_H
#define PSPEXP_TERM_SYSINFO_H

#include <stdint.h>
#include <sys/types.h>

#define SYS_CORES 8
#define SYS_NETS  6
#define SYS_PROCS 96

typedef struct SysProc {
    int pid, uid;
    char name[40];
    double cpu;          /* percent of one core */
    uint64_t rss;        /* bytes */
} SysProc;

typedef struct SysNet {
    char name[16], addr[48];
    double rx_bps, tx_bps;
    int wifi;            /* link quality 0-100, -1 not Wi-Fi */
} SysNet;

typedef struct SysInfo {
    char host[64], model[96], kernel[80];
    double uptime, load[3];
    int ncpu, mhz;                   /* mhz 0: unknown */
    double cpu, core[SYS_CORES];     /* percent busy: all, per core */
    int temp_mc;                     /* millidegrees C, -1 unknown */
    uint64_t mem_total, mem_avail, swap_total, swap_free;
    SysNet net[SYS_NETS]; int nnet;
    SysProc proc[SYS_PROCS]; int nproc;   /* busiest first */
} SysInfo;

void sysinfo_update(SysInfo *s);
/* end pid: SIGTERM, or SIGKILL with force. 0 or -1 with errno */
int  sysinfo_end(int pid, int force);

#endif
