/* About and the status bar on a Mac: see launcher_mac.h */
#include "launcher_mac.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>

static void sysctl_str(const char *name, char *out, size_t n)
{
    size_t len = n;
    out[0] = 0;
    if (sysctlbyname(name, out, &len, NULL, 0) != 0) out[0] = 0;
    else out[n - 1] = 0;
}

static int sysctl_int(const char *name)
{
    int v = 0;
    size_t len = sizeof(v);
    return sysctlbyname(name, &v, &len, NULL, 0) == 0 ? v : 0;
}

void mac_model(char *out, size_t n)
{
    /* the name Apple sells it by, in the device tree (Apple silicon) */
    io_registry_entry_t e = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/product");
    out[0] = 0;
    if (e) {
        CFTypeRef v = IORegistryEntryCreateCFProperty(e, CFSTR("product-name"), kCFAllocatorDefault, 0);
        if (v && CFGetTypeID(v) == CFDataGetTypeID()) {
            CFIndex l = CFDataGetLength(v);
            if ((size_t)l >= n) l = (CFIndex)n - 1;
            memcpy(out, CFDataGetBytePtr(v), (size_t)l);
            out[l] = 0;
        }
        if (v) CFRelease(v);
        IOObjectRelease(e);
    }
    if (!out[0]) sysctl_str("hw.model", out, n);   /* Intel Macs: Mac14,2 */
    if (!out[0]) snprintf(out, n, "Mac");
}

void mac_chip(char *out, size_t n)
{
    sysctl_str("machdep.cpu.brand_string", out, n);
}

void mac_cores(char *out, size_t n)
{
    struct utsname u;
    int all = sysctl_int("hw.ncpu"), p = sysctl_int("hw.perflevel0.logicalcpu"), e = sysctl_int("hw.perflevel1.logicalcpu");
    uname(&u);
    if (p && e) snprintf(out, n, "%d cores (%dP + %dE) %s", all, p, e, u.machine);
    else snprintf(out, n, "%d core%s %s", all, all == 1 ? "" : "s", u.machine);
}

void mac_memory(unsigned long long *total, unsigned long long *avail)
{
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    vm_statistics64_data_t vs;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    *total = sysctlbyname("hw.memsize", &mem, &len, NULL, 0) == 0 ? mem : 0;
    *avail = 0;
    host_page_size(mach_host_self(), &page);
    /* what Activity Monitor would hand out without swapping: free, inactive, purgeable */
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &cnt) == KERN_SUCCESS)
        *avail = (unsigned long long)(vs.free_count + vs.inactive_count + vs.purgeable_count) * page;
}

void mac_os(char *os, size_t n, char *kernel, size_t kn)
{
    char v[32];
    struct utsname u;
    sysctl_str("kern.osproductversion", v, sizeof(v));
    snprintf(os, n, "macOS %s", v[0] ? v : "");
    uname(&u);
    snprintf(kernel, kn, "Darwin %s", u.release);
}

long mac_uptime(void)
{
    struct timeval boot;
    size_t len = sizeof(boot);
    if (sysctlbyname("kern.boottime", &boot, &len, NULL, 0) != 0) return -1;
    return (long)(time(NULL) - boot.tv_sec);
}

int mac_battery(int *charging)
{
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    CFArrayRef list = info ? IOPSCopyPowerSourcesList(info) : NULL;
    int level = -1;
    CFIndex i;
    *charging = 0;
    for (i = 0; list && i < CFArrayGetCount(list) && level < 0; i++) {
        CFDictionaryRef d = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
        CFStringRef type = d ? CFDictionaryGetValue(d, CFSTR(kIOPSTypeKey)) : NULL;
        CFNumberRef cur, max;
        CFBooleanRef chg;
        int c = 0, m = 0;
        if (!type || CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0) != kCFCompareEqualTo) continue;
        cur = CFDictionaryGetValue(d, CFSTR(kIOPSCurrentCapacityKey));
        max = CFDictionaryGetValue(d, CFSTR(kIOPSMaxCapacityKey));
        if (cur) CFNumberGetValue(cur, kCFNumberIntType, &c);
        if (max) CFNumberGetValue(max, kCFNumberIntType, &m);
        level = m > 0 ? c * 100 / m : c;
        chg = CFDictionaryGetValue(d, CFSTR(kIOPSIsChargingKey));
        *charging = chg && CFBooleanGetValue(chg);
    }
    if (list) CFRelease(list);
    if (info) CFRelease(info);
    return level;
}
