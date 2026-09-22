#ifndef PSPEXP_TIME_H
#define PSPEXP_TIME_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Monotonic host clock in microseconds. */
uint64_t pspexp_time_us(void);
void     pspexp_sleep_us(uint32_t us);
#ifdef __cplusplus
}
#endif
#endif
