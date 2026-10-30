#include "hle_stub.h"
#include "hle_call.h"
#include "guest_mem.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STUBS 8192
typedef struct { char *name; void *fn; int forced; } hle_entry_t;
static hle_entry_t g_reg[MAX_STUBS];    // registered functions, by name
static int g_nreg;
static hle_entry_t g_stub[MAX_STUBS];   // stub i: name and function (fn NULL = unimplemented)
static int g_nstub;
static uint64_t g_page;                 // guest address of stub 0; stub i is 8 bytes at g_page + 8*i
unsigned long hle_unimplemented_calls;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;     // registration and stub creation; a stub is immutable once made, so dispatch takes no lock

void hle_register(const char *name, void *fn) {
    pthread_mutex_lock(&g_mu);
    if (g_nreg == MAX_STUBS) { fprintf(stderr, "[RED] hle_register: table full\n"); abort(); }
    g_reg[g_nreg].name = strdup(name); g_reg[g_nreg].fn = fn; g_nreg++;
    pthread_mutex_unlock(&g_mu);
}

void hle_register_force(const char *name, void *fn) {
    hle_register(name, fn);
    pthread_mutex_lock(&g_mu); g_reg[g_nreg - 1].forced = 1; pthread_mutex_unlock(&g_mu);
}
int hle_is_forced(const char *name) {
    pthread_mutex_lock(&g_mu);
    int r = 0; for (int i = 0; i < g_nreg && !r; i++) r = g_reg[i].forced && !strcmp(g_reg[i].name, name);
    pthread_mutex_unlock(&g_mu);
    return r;
}

static void *lookup(const char *name) {
    for (int i = 0; i < g_nreg; i++) if (!strcmp(g_reg[i].name, name)) return g_reg[i].fn;
    return NULL;
}
int hle_implements(const char *name) {
    pthread_mutex_lock(&g_mu); int r = lookup(name) != NULL; pthread_mutex_unlock(&g_mu);
    return r;
}

uint64_t hle_stub_for(const char *name) {
    pthread_mutex_lock(&g_mu);
    if (!g_page) g_page = guest_alloc(MAX_STUBS * 8, 0x1000);
    if (!g_page) { pthread_mutex_unlock(&g_mu); return 0; }
    for (int i = 0; i < g_nstub; i++) if (!strcmp(g_stub[i].name, name)) { pthread_mutex_unlock(&g_mu); return g_page + 8 * i; }
    if (g_nstub == MAX_STUBS) { fprintf(stderr, "[RED] hle_stub_for: stub page full\n"); pthread_mutex_unlock(&g_mu); return 0; }
    int i = g_nstub++;
    g_stub[i].name = strdup(name);
    g_stub[i].fn = lookup(name);
    uint32_t *code = (uint32_t *)(g_page + 8 * i);
    if (cpu_is_aarch32()) { code[0] = 0xEF000000u | (HLE_SVC_BASE + i); code[1] = 0xE12FFF1Eu; }   // svc #(HLE_SVC_BASE+i); bx lr
    else { code[0] = 0xD4000001u | ((HLE_SVC_BASE + i) << 5); code[1] = 0xD65F03C0u; }              // svc #(HLE_SVC_BASE+i); ret
    uint64_t a = g_page + 8 * i;
    pthread_mutex_unlock(&g_mu);
    return a;
}

const char *hle_stub_name(uint32_t swi) { return swi >= HLE_SVC_BASE && swi < HLE_SVC_BASE + (uint32_t)g_nstub ? g_stub[swi - HLE_SVC_BASE].name : NULL; }

int hle_dispatch(cpu_t *c, uint32_t swi) {
    if (swi < HLE_SVC_BASE || swi >= HLE_SVC_BASE + (uint32_t)g_nstub) return 0;
    hle_entry_t *e = &g_stub[swi - HLE_SVC_BASE];
    if (!e->fn) {
        hle_unimplemented_calls++;
        fprintf(stderr, "[RED] unimplemented import %s called (lr=%#llx)\n", e->name, (unsigned long long)cpu_get_x(c, 30));
        cpu_set_x(c, 0, 0);
        return 1;
    }
    hle_regs_t r;
    for (int i = 0; i < 8; i++) { r.x[i] = cpu_get_x(c, i); uint64_t v[2]; cpu_get_v(c, i, v); r.d[i] = v[0]; }
    hle_call(e->fn, &r);
    cpu_set_x(c, 0, r.x[0]);
    uint64_t v[2] = {r.d[0], 0};
    cpu_set_v(c, 0, v);
    return 1;
}
