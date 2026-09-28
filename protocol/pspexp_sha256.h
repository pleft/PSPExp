/* SHA-256 (FIPS 180-4), small and dependency-free: the PSP's boot loader
 * checks a runtime slot's files with it, and the console the files it
 * copies to the PSP (docs/Package_Manager.md §6.1). */
#ifndef PSPEXP_SHA256_H
#define PSPEXP_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct PspExpSha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t used; } PspExpSha256;

void pspexp_sha256_init(PspExpSha256 *s);
void pspexp_sha256_update(PspExpSha256 *s, const void *data, size_t n);
void pspexp_sha256_final(PspExpSha256 *s, uint8_t out[32]);
/* the digest of data as 64 lower-case hex characters and a NUL */
void pspexp_sha256_hex(const uint8_t digest[32], char out[65]);

#endif
