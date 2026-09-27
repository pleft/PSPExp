/*
 * About and the status bar on a Mac (the development host): what Linux boards
 * read from /proc and /sys, from sysctl and IOKit.
 */
#ifndef PSPEXP_LAUNCHER_MAC_H
#define PSPEXP_LAUNCHER_MAC_H

#include <stddef.h>

void mac_model(char *out, size_t n);     /* MacBook Air (13-inch, M5) */
void mac_chip(char *out, size_t n);      /* Apple M5 */
void mac_cores(char *out, size_t n);     /* 10 cores (4P + 6E) arm64 */
void mac_memory(unsigned long long *total, unsigned long long *avail);
void mac_os(char *os, size_t n, char *kernel, size_t kn);   /* macOS 27.0, Darwin 27.0.0 */
long mac_uptime(void);                   /* seconds since boot */
/* the internal battery: its level, -1 when there is none */
int  mac_battery(int *charging);

#endif
