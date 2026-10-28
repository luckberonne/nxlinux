#include "../src/host/hle_call.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

static uint64_t sum8(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}
static double mix(uint32_t i, double a, float b, const char *s, double c) { return i + a + b + c + (double)strlen(s); }
static float fret(float a, float b) { return a * b; }
static int32_t neg(int32_t v) { return -v; }
static void sink(uint32_t *out) { *out = 0xC0FFEE; }

int main(void) {
    hle_regs_t r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < 8; i++) r.x[i] = i + 1;
    hle_call((void *)sum8, &r);
    CHECK(r.x[0] == 1 + 4 + 9 + 16 + 25 + 36 + 49 + 64);

    double a = 1.5, c = 0.25; float b = 2.0f;
    memset(&r, 0, sizeof r);
    r.x[0] = 10; r.x[1] = (uint64_t)"abcd";
    memcpy(&r.d[0], &a, 8); memcpy(&r.d[1], &b, 4); memcpy(&r.d[2], &c, 8);
    hle_call((void *)mix, &r);
    double res; memcpy(&res, &r.d[0], 8);
    CHECK(res == 10 + 1.5 + 2.0 + 0.25 + 4);

    memset(&r, 0, sizeof r);
    float f1 = 3.0f, f2 = 4.0f; memcpy(&r.d[0], &f1, 4); memcpy(&r.d[1], &f2, 4);
    hle_call((void *)fret, &r);
    float fr; memcpy(&fr, &r.d[0], 4);
    CHECK(fr == 12.0f);

    memset(&r, 0, sizeof r);
    r.x[0] = 0xFFFFFFFF00000005ull;                  // guest left garbage in the upper half of a W register
    hle_call((void *)neg, &r);
    CHECK((int32_t)r.x[0] == -5);

    uint32_t out = 0;
    memset(&r, 0, sizeof r); r.x[0] = (uint64_t)&out;
    hle_call((void *)sink, &r);
    CHECK(out == 0xC0FFEE);

    puts(g_fail ? "FAIL" : "PASS");
    return g_fail;
}
