#include "nvn_shader_vk.h"
#include "nvn_vk_priv.h"
#include "nvn_draw.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct nvn_shader {
    uint8_t key[8];
    int stage; uint32_t needs;
    uint32_t *spirv; size_t bytes;
    nvn_shbind *binds; int nbinds;
    uint8_t *c1; size_t c1_bytes; vkbuf_t c1buf; int c1made;   // c[1] and the uniform buffer holding it, made on first use
    VkShaderModule module;                 // made on first use
    struct nvn_shader *next, *hnext;       // next: registration order (kept for nothing but memory); hnext: the key's bucket
} nvn_shader;

static nvn_shader *g_head;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
// Every draw resolves two shaders by key, and a course's library holds thousands: the registry is a hash table on the key, with a one-entry cache per
// thread because consecutive draws usually use the same program. A linear walk here cost about 100 us per draw with 2,839 shaders (each hop a cache miss).
enum { SHADER_BUCKETS = 8192 };
static nvn_shader *g_shash[SHADER_BUCKETS];
static unsigned shader_bucket(uint64_t v) { v *= 0x9E3779B97F4A7C15ull; return (unsigned)(v >> 51); }   // 8192 = 2^13: the key's high bits after mixing

uint64_t nvn_vk_shader_key(const uint8_t key[8]) { uint64_t v; memcpy(&v, key, 8); return v; }

void nvn_vk_shader_register(const uint8_t key[8], const nvn_shader_desc *d) {
    nvn_shader *s = calloc(1, sizeof *s);
    memcpy(s->key, key, 8); s->stage = d->stage; s->needs = d->needs;
    s->spirv = malloc(d->bytes); memcpy(s->spirv, d->spirv, d->bytes); s->bytes = d->bytes;
    if (d->c1_bytes) { s->c1 = malloc(d->c1_bytes); memcpy(s->c1, d->c1, d->c1_bytes); s->c1_bytes = d->c1_bytes; }
    s->binds = malloc((d->nbinds ? d->nbinds : 1) * sizeof *s->binds); if (d->nbinds) memcpy(s->binds, d->binds, d->nbinds * sizeof *s->binds); s->nbinds = d->nbinds;
    uint64_t kv; memcpy(&kv, key, 8); unsigned b = shader_bucket(kv);
    pthread_mutex_lock(&g_mu);
    for (nvn_shader **p = &g_shash[b]; *p; p = &(*p)->hnext)
        if (!memcmp((*p)->key, key, 8)) { s->hnext = (*p)->hnext; /* the old one may still be in a pipeline: it is leaked on purpose */ *p = s; s->next = g_head; g_head = s; pthread_mutex_unlock(&g_mu); return; }
    s->hnext = g_shash[b]; g_shash[b] = s; s->next = g_head; g_head = s;
    pthread_mutex_unlock(&g_mu);
}

const nvn_shader *nvn_shader_find(const uint8_t key[8]) {
    static _Thread_local nvn_shader *last; static _Thread_local uint64_t lastv;
    uint64_t kv; memcpy(&kv, key, 8);
    if (last && lastv == kv) return last;
    unsigned b = shader_bucket(kv);
    pthread_mutex_lock(&g_mu);
    nvn_shader *s = g_shash[b];
    while (s && memcmp(s->key, key, 8)) s = s->hnext;
    pthread_mutex_unlock(&g_mu);
    if (s) { last = s; lastv = kv; }
    return s;
}
int nvn_shader_stage(const nvn_shader *s) { return s->stage; }
uint32_t nvn_shader_needs(const nvn_shader *s) { return s->needs; }
const nvn_shbind *nvn_shader_binds(const nvn_shader *s, int *n) { *n = s->nbinds; return s->binds; }
enum { C1_SIZE = 4096 };                   // the buffer covers what a uniform block of the translator may declare beyond the 0x100 captured bytes (zeros)
VkDescriptorBufferInfo nvn_shader_c1(const nvn_shader *cs) {
    nvn_shader *s = (nvn_shader *)cs;
    if (!__atomic_load_n(&s->c1made, __ATOMIC_ACQUIRE)) {       // every draw binds it: the lock is taken once, at the first use
        pthread_mutex_lock(&g_mu);
        if (!s->c1made) {
            vkctx_t *c = nvn_vk_ctx();
            if (c && !vkbuf_create(c, &s->c1buf, C1_SIZE, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1)) { memset(s->c1buf.map, 0, C1_SIZE); memcpy(s->c1buf.map, s->c1, s->c1_bytes < C1_SIZE ? s->c1_bytes : C1_SIZE); }
            else s->c1buf.buf = VK_NULL_HANDLE;
            __atomic_store_n(&s->c1made, 1, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&g_mu);
    }
    return (VkDescriptorBufferInfo){s->c1buf.buf, 0, C1_SIZE};
}
VkShaderModule nvn_shader_module(nvn_shader *s) {
    pthread_mutex_lock(&g_mu);
    if (!s->module) {
        vkctx_t *c = nvn_vk_ctx();
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = s->bytes, .pCode = s->spirv};
        if (!c || vkCreateShaderModule(c->dev, &ci, NULL, &s->module)) s->module = VK_NULL_HANDLE;
    }
    VkShaderModule m = s->module;
    pthread_mutex_unlock(&g_mu);
    return m;
}

// ---- libraries
int nvn_vk_shader_load_library(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[dm] [RED] [nvn] shader library %s: %s\n", path, strerror(errno)); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n ? (size_t)n : 1);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return -1; }
    fclose(f);
    int count = -1;
    uint32_t ver, cnt;
    if (n < 12 || memcmp(d, "NXSL", 4)) { fprintf(stderr, "[dm] [RED] [nvn] %s is not a shader library\n", path); goto done; }
    memcpy(&ver, d + 4, 4); memcpy(&cnt, d + 8, 4);
    if (ver != 2 && ver != 3) { fprintf(stderr, "[dm] [RED] [nvn] shader library %s has version %u, expected 2 or 3\n", path, ver); goto done; }
    size_t off = 12; count = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t h[4];
        if (off + 24 > (size_t)n) { fprintf(stderr, "[dm] [RED] [nvn] shader library %s is cut short\n", path); count = -1; goto done; }
        const uint8_t *key = d + off; memcpy(h, d + off + 8, 16); off += 24;
        uint32_t stage = h[0], needs = h[1], nb = h[2], nbytes = h[3];
        if (nb > 256 || (size_t)nb * 16 + nbytes > (size_t)n - off || (nbytes & 3)) { fprintf(stderr, "[dm] [RED] [nvn] shader library %s: entry %u is damaged\n", path, i); count = -1; goto done; }
        nvn_shbind *binds = malloc((nb ? nb : 1) * sizeof *binds);
        for (uint32_t b = 0; b < nb; b++) memcpy(&binds[b], d + off + 16 * b, 16);
        off += (size_t)nb * 16;
        const uint8_t *c1 = NULL; uint32_t n1 = 0;
        if (ver == 3) {                                                  // the constant segment c[1]
            if (off + 4 > (size_t)n) { free(binds); fprintf(stderr, "[dm] [RED] [nvn] shader library %s: entry %u is damaged\n", path, i); count = -1; goto done; }
            memcpy(&n1, d + off, 4); off += 4;
            if (n1 > (size_t)n - off || nbytes > (size_t)n - off - n1) { free(binds); fprintf(stderr, "[dm] [RED] [nvn] shader library %s: entry %u is damaged\n", path, i); count = -1; goto done; }
            c1 = d + off; off += n1;
        }
        uint32_t *code = malloc(nbytes); memcpy(code, d + off, nbytes); off += nbytes;
        nvn_shader_desc desc = {(int)stage, needs, code, nbytes, binds, (int)nb, c1, n1};
        nvn_vk_shader_register(key, &desc);
        free(binds); free(code); count++;
    }
done:
    free(d);
    return count;
}
