/* The system monitor's figures: see sysinfo.h */
#define _GNU_SOURCE
#include "sysinfo.h"

#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <net/if_dl.h>
#include <sys/sysctl.h>
#endif

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int cmp_proc(const void *a, const void *b)
{
    const SysProc *x = a, *y = b;
    if (x->cpu != y->cpu) return x->cpu < y->cpu ? 1 : -1;
    return x->rss < y->rss ? 1 : x->rss > y->rss ? -1 : 0;
}

/* per process: the CPU time it had last time (ticks on Linux, ns on macOS) */
#define PREV_MAX 1024
static struct { int pid; uint64_t t; } g_prev[PREV_MAX], g_cur[PREV_MAX];
static int g_nprev, g_ncur;
static double g_prev_at;

static uint64_t prev_time(int pid)
{
    int i;
    for (i = 0; i < g_nprev; i++) if (g_prev[i].pid == pid) return g_prev[i].t;
    return 0;
}

static void keep_time(int pid, uint64_t t)
{
    if (g_ncur < PREV_MAX) { g_cur[g_ncur].pid = pid; g_cur[g_ncur].t = t; g_ncur++; }
}

/* the network: addresses from getifaddrs; byte counts per system */
static struct { char name[16]; uint64_t rx, tx; } g_nprevc[SYS_NETS];
static double g_net_at;

/* the interfaces with an IPv4 address now (the others are not shown: a Mac has a dozen) */
static int has_ipv4(const char *name)
{
    struct ifaddrs *ifa, *p;
    int yes = 0;
    if (getifaddrs(&ifa) != 0) return 1;
    for (p = ifa; p && !yes; p = p->ifa_next) yes = p->ifa_addr && p->ifa_addr->sa_family == AF_INET && !strcmp(p->ifa_name, name);
    freeifaddrs(ifa);
    return yes;
}

static void net_rates(SysInfo *s, const char *name, uint64_t rx, uint64_t tx, double dt)
{
    int i, k = -1;
    SysNet *n;
    if (s->nnet >= SYS_NETS || !strcmp(name, "lo") || !strncmp(name, "lo0", 3)) return;
    if (!has_ipv4(name) && strncmp(name, "wl", 2)) return;   /* Wi-Fi shows even while it has no address */
    n = &s->net[s->nnet++];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->wifi = -1;
    for (i = 0; i < SYS_NETS; i++) if (!strcmp(g_nprevc[i].name, name)) k = i;
    if (k >= 0 && dt > 0 && rx >= g_nprevc[k].rx && tx >= g_nprevc[k].tx) {
        n->rx_bps = (rx - g_nprevc[k].rx) / dt;
        n->tx_bps = (tx - g_nprevc[k].tx) / dt;
    }
    if (k < 0) for (i = 0; i < SYS_NETS; i++) if (!g_nprevc[i].name[0]) { k = i; break; }
    if (k >= 0) { snprintf(g_nprevc[k].name, sizeof(g_nprevc[k].name), "%s", name); g_nprevc[k].rx = rx; g_nprevc[k].tx = tx; }
}

static void net_addrs(SysInfo *s)
{
    struct ifaddrs *ifa, *p;
    int i;
    if (getifaddrs(&ifa) != 0) return;
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        for (i = 0; i < s->nnet; i++)
            if (!strcmp(s->net[i].name, p->ifa_name) && !s->net[i].addr[0])
                inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, s->net[i].addr, sizeof(s->net[i].addr));
    }
    freeifaddrs(ifa);
}

/* the interfaces worth a line: up with an address, or Wi-Fi */
static void net_trim(SysInfo *s)
{
    int i, k = 0;
    for (i = 0; i < s->nnet; i++)
        if (s->net[i].addr[0] || s->net[i].wifi >= 0) s->net[k++] = s->net[i];
    s->nnet = k;
}

#if defined(__linux__)

static int read_text(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    size_t l;
    if (!f) return -1;
    l = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[l] = 0;
    while (l && (buf[l - 1] == '\n' || buf[l - 1] == ' ' || buf[l - 1] == 0)) buf[--l] = 0;
    return (int)l;
}

static uint64_t g_cpu_prev[SYS_CORES + 1][2];   /* busy, total */

static void cpu_linux(SysInfo *s)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[512];
    if (!f) return;
    s->ncpu = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long v[8] = { 0 };
        int idx;
        if (strncmp(line, "cpu", 3)) break;
        if (line[3] == ' ') idx = 0;
        else { idx = atoi(line + 3) + 1; if (idx > SYS_CORES) continue; s->ncpu++; }
        sscanf(strchr(line, ' '), "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        {
            uint64_t idle = v[3] + v[4], total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7], busy = total - idle;
            double pct = 0;
            if (g_cpu_prev[idx][1] && total > g_cpu_prev[idx][1]) pct = 100.0 * (busy - g_cpu_prev[idx][0]) / (total - g_cpu_prev[idx][1]);
            g_cpu_prev[idx][0] = busy; g_cpu_prev[idx][1] = total;
            if (idx == 0) s->cpu = pct; else s->core[idx - 1] = pct;
        }
    }
    fclose(f);
}

static void procs_linux(SysInfo *s, double dt)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    long hz = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
    g_ncur = 0;
    if (!d) return;
    while ((de = readdir(d))) {
        char path[64], buf[1024], *lp, *rp;
        struct stat st;
        unsigned long long ut = 0, stt = 0;
        long rss = 0;
        int pid = atoi(de->d_name);
        SysProc *p;
        if (pid <= 0) continue;
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        if (read_text(path, buf, sizeof(buf)) <= 0) continue;
        if (!(lp = strchr(buf, '(')) || !(rp = strrchr(buf, ')'))) continue;
        /* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime ... rss is field 24 */
        if (sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %*d %*d %*u %*u %ld", &ut, &stt, &rss) < 3) continue;
        if (rss <= 0) continue;   /* kernel threads */
        keep_time(pid, ut + stt);
        if (s->nproc >= SYS_PROCS) {   /* full: replace the idlest when this one is busier */
            int k, low = 0;
            double cpu = dt > 0 && prev_time(pid) ? (ut + stt - prev_time(pid)) * 100.0 / hz / dt : 0;
            for (k = 1; k < s->nproc; k++) if (s->proc[k].cpu < s->proc[low].cpu) low = k;
            if (cpu <= s->proc[low].cpu) continue;
            p = &s->proc[low];
        } else p = &s->proc[s->nproc++];
        memset(p, 0, sizeof(*p));
        p->pid = pid;
        snprintf(p->name, sizeof(p->name), "%.*s", (int)(rp - lp - 1) < 39 ? (int)(rp - lp - 1) : 39, lp + 1);
        p->rss = (uint64_t)rss * (uint64_t)page;
        snprintf(path, sizeof(path), "/proc/%d", pid);
        p->uid = stat(path, &st) == 0 ? (int)st.st_uid : -1;
        if (dt > 0 && prev_time(pid) && ut + stt >= prev_time(pid)) p->cpu = (ut + stt - prev_time(pid)) * 100.0 / hz / dt;
    }
    closedir(d);
}

static void net_linux(SysInfo *s, double dt)
{
    FILE *f = fopen("/proc/net/dev", "r");
    char line[512];
    int i;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *c = strchr(line, ':'), *n = line;
        unsigned long long v[16] = { 0 };
        if (!c) continue;
        *c = 0;
        while (*n == ' ') n++;
        sscanf(c + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]);
        net_rates(s, n, v[0], v[8], dt);
    }
    fclose(f);
    if ((f = fopen("/proc/net/wireless", "r"))) {   /* "wlan0: 0000   54.  -56.  ..." : link quality out of 70 */
        while (fgets(line, sizeof(line), f)) {
            char *c = strchr(line, ':'), *n = line;
            float q = 0;
            if (!c) continue;
            *c = 0;
            while (*n == ' ') n++;
            if (sscanf(c + 1, "%*x %f", &q) != 1) continue;
            for (i = 0; i < s->nnet; i++) if (!strcmp(s->net[i].name, n)) s->net[i].wifi = q > 70 ? 100 : (int)(q * 100 / 70);
        }
        fclose(f);
    }
}

void sysinfo_update(SysInfo *s)
{
    char buf[256];
    struct utsname u;
    double now = mono(), dt = g_prev_at ? now - g_prev_at : 0, ndt = g_net_at ? now - g_net_at : 0;
    int i;
    memset(s, 0, sizeof(*s));
    gethostname(s->host, sizeof(s->host) - 1);
    if (uname(&u) == 0) snprintf(s->kernel, sizeof(s->kernel), "%s %s", u.sysname, u.release);
    if (read_text("/proc/device-tree/model", s->model, sizeof(s->model)) <= 0 &&
        read_text("/tmp/sysinfo/model", s->model, sizeof(s->model)) <= 0) snprintf(s->model, sizeof(s->model), "%s", u.machine);
    if (read_text("/proc/uptime", buf, sizeof(buf)) > 0) s->uptime = atof(buf);
    getloadavg(s->load, 3);
    cpu_linux(s);
    if (read_text("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", buf, sizeof(buf)) > 0) s->mhz = atoi(buf) / 1000;
    s->temp_mc = -1;
    for (i = 0; i < 4 && s->temp_mc < 0; i++) {
        char p[80];
        snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/temp", i);
        if (read_text(p, buf, sizeof(buf)) > 0 && atoi(buf) > 0) s->temp_mc = atoi(buf);
    }
    {
        FILE *f = fopen("/proc/meminfo", "r");
        char line[256];
        unsigned long long kb, fr = 0, buffers = 0, cached = 0;
        int have_avail = 0;
        while (f && fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemTotal: %llu", &kb) == 1) s->mem_total = kb * 1024;
            else if (sscanf(line, "MemAvailable: %llu", &kb) == 1) { s->mem_avail = kb * 1024; have_avail = 1; }
            else if (sscanf(line, "MemFree: %llu", &kb) == 1) fr = kb;
            else if (sscanf(line, "Buffers: %llu", &kb) == 1) buffers = kb;
            else if (sscanf(line, "Cached: %llu", &kb) == 1) cached = kb;
            else if (sscanf(line, "SwapTotal: %llu", &kb) == 1) s->swap_total = kb * 1024;
            else if (sscanf(line, "SwapFree: %llu", &kb) == 1) s->swap_free = kb * 1024;
        }
        if (f) fclose(f);
        if (!have_avail) s->mem_avail = (fr + buffers + cached) * 1024;   /* old kernels (the Omega's) */
    }
    net_linux(s, ndt);
    net_addrs(s);
    net_trim(s);
    g_net_at = now;
    procs_linux(s, dt);
    memcpy(g_prev, g_cur, sizeof(g_cur[0]) * (size_t)g_ncur); g_nprev = g_ncur;
    g_prev_at = now;
    qsort(s->proc, (size_t)s->nproc, sizeof(SysProc), cmp_proc);
}

#elif defined(__APPLE__)

static uint64_t g_mcpu_prev[SYS_CORES + 1][2];

void sysinfo_update(SysInfo *s)
{
    double now = mono(), dt = g_prev_at ? now - g_prev_at : 0, ndt = g_net_at ? now - g_net_at : 0;
    struct utsname u;
    size_t len;
    int i, n;
    memset(s, 0, sizeof(*s));
    gethostname(s->host, sizeof(s->host) - 1);
    if (uname(&u) == 0) snprintf(s->kernel, sizeof(s->kernel), "%s %s", u.sysname, u.release);
    len = sizeof(s->model);
    if (sysctlbyname("hw.model", s->model, &len, NULL, 0) != 0) snprintf(s->model, sizeof(s->model), "Mac");
    {
        struct timeval bt; len = sizeof(bt);
        if (sysctlbyname("kern.boottime", &bt, &len, NULL, 0) == 0) s->uptime = (double)(time(NULL) - bt.tv_sec);
    }
    getloadavg(s->load, 3);
    s->temp_mc = -1;
    {
        uint64_t mem; len = sizeof(mem);
        if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) == 0) s->mem_total = mem;
    }
    {
        vm_statistics64_data_t vm;
        mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
        vm_size_t page = 4096;
        host_page_size(mach_host_self(), &page);
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &cnt) == KERN_SUCCESS)
            s->mem_avail = (uint64_t)(vm.free_count + vm.inactive_count + vm.speculative_count) * page;
    }
    {
        natural_t ncpu = 0;
        processor_info_array_t info;
        mach_msg_type_number_t ninfo;
        if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &ncpu, &info, &ninfo) == KERN_SUCCESS) {
            uint64_t tb = 0, tt = 0;
            s->ncpu = ncpu > SYS_CORES ? SYS_CORES : (int)ncpu;
            for (i = 0; i < (int)ncpu; i++) {
                unsigned *t = (unsigned *)&info[i * CPU_STATE_MAX];
                uint64_t busy = t[CPU_STATE_USER] + t[CPU_STATE_SYSTEM] + t[CPU_STATE_NICE], total = busy + t[CPU_STATE_IDLE];
                tb += busy; tt += total;
                if (i < SYS_CORES) {
                    if (g_mcpu_prev[i + 1][1] && total > g_mcpu_prev[i + 1][1]) s->core[i] = 100.0 * (busy - g_mcpu_prev[i + 1][0]) / (total - g_mcpu_prev[i + 1][1]);
                    g_mcpu_prev[i + 1][0] = busy; g_mcpu_prev[i + 1][1] = total;
                }
            }
            if (g_mcpu_prev[0][1] && tt > g_mcpu_prev[0][1]) s->cpu = 100.0 * (tb - g_mcpu_prev[0][0]) / (tt - g_mcpu_prev[0][1]);
            g_mcpu_prev[0][0] = tb; g_mcpu_prev[0][1] = tt;
            vm_deallocate(mach_task_self(), (vm_address_t)info, ninfo * sizeof(int));
        }
    }
    {   /* bytes per interface: the AF_LINK entries carry if_data */
        struct ifaddrs *ifa, *p;
        if (getifaddrs(&ifa) == 0) {
            for (p = ifa; p; p = p->ifa_next)
                if (p->ifa_addr && p->ifa_addr->sa_family == AF_LINK && p->ifa_data && (p->ifa_flags & IFF_UP)) {
                    struct if_data *d = (struct if_data *)p->ifa_data;
                    net_rates(s, p->ifa_name, d->ifi_ibytes, d->ifi_obytes, ndt);
                }
            freeifaddrs(ifa);
        }
        net_addrs(s);
        net_trim(s);
        g_net_at = now;
    }
    {
        static pid_t pids[4096];
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        g_ncur = 0;
        n = proc_listallpids(pids, sizeof(pids));
        for (i = 0; i < n; i++) {
            struct proc_taskinfo ti;
            struct proc_bsdshortinfo bi;
            uint64_t t;
            SysProc *p;
            if (pids[i] <= 0 || proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) != sizeof(ti)) continue;
            t = (ti.pti_total_user + ti.pti_total_system) * tb.numer / tb.denom;   /* ns */
            keep_time(pids[i], t);
            if (s->nproc >= SYS_PROCS) continue;
            p = &s->proc[s->nproc++];
            memset(p, 0, sizeof(*p));
            p->pid = pids[i];
            proc_name(pids[i], p->name, sizeof(p->name));
            p->rss = ti.pti_resident_size;
            p->uid = proc_pidinfo(pids[i], PROC_PIDT_SHORTBSDINFO, 0, &bi, sizeof(bi)) == sizeof(bi) ? (int)bi.pbsi_uid : -1;
            if (dt > 0 && prev_time(pids[i]) && t >= prev_time(pids[i])) p->cpu = (t - prev_time(pids[i])) / 1e9 / dt * 100.0;
        }
        memcpy(g_prev, g_cur, sizeof(g_cur[0]) * (size_t)g_ncur); g_nprev = g_ncur;
        g_prev_at = now;
        qsort(s->proc, (size_t)s->nproc, sizeof(SysProc), cmp_proc);
    }
}

#else
void sysinfo_update(SysInfo *s) { memset(s, 0, sizeof(*s)); s->temp_mc = -1; gethostname(s->host, sizeof(s->host) - 1); }
#endif

int sysinfo_end(int pid, int force)
{
    if (pid <= 1 || pid == getpid()) { errno = EPERM; return -1; }
    return kill(pid, force ? SIGKILL : SIGTERM);
}
