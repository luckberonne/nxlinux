// SHA-256 (FIPS 180-4). Program keys are the first 8 bytes of SHA-256 over a shader's control section and code, the same key the offline shader tools use.
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t n);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
