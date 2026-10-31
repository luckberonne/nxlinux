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
enum { SHADER_BUCKETS = 32768 };
static nvn_shader *g_shash[SHADER_BUCKETS];
static unsigned shader_bucket(uint64_t v) { v *= 0x9E3779B97F4A7C15ull; return (unsigned)(v >> 49); }   // 32768 = 2^15: the key's high bits after mixing

// A shader found by the hash of its code alone (hle_nvn.c prog_alt_key) is also registered under the program's own key, so the next draw of that program finds it by one lookup
typedef struct alias { uint64_t key; nvn_shader *s; struct alias *next; } alias_t;
static alias_t *g_ahash[SHADER_BUCKETS];
void nvn_shader_alias(const uint8_t key[8], const nvn_shader *s) {
    uint64_t kv; memcpy(&kv, key, 8); unsigned b = shader_bucket(kv);
    pthread_mutex_lock(&g_mu);
    alias_t *a = g_ahash[b]; while (a && a->key != kv) a = a->next;
    if (!a) { a = malloc(sizeof *a); if (a) { a->key = kv; a->next = g_ahash[b]; g_ahash[b] = a; } }
    if (a) a->s = (nvn_shader *)s;
    pthread_mutex_unlock(&g_mu);
}
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
    if (!s) { alias_t *a = g_ahash[b]; while (a && a->key != kv) a = a->next; if (a) s = a->s; }
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
// A title that draws a depth pass and then the colour pass with the depth test EQUAL (Mario Kart 8 Deluxe) needs the same vertex position from two different shaders, bit for bit. The hardware
// computes it the same way in both; Vulkan only promises that for a position decorated Invariant. The translator does not decorate it, so the SPIR-V of a vertex shader is patched here:
// every OpMemberDecorate <struct> <n> BuiltIn Position (and OpDecorate <var> BuiltIn Position) gets an Invariant decoration right after it.
static uint32_t *spirv_invariant_position(const uint32_t *in, size_t bytes, size_t *outbytes) {
    size_t nw = bytes / 4; *outbytes = bytes;
    if (nw < 5 || in[0] != 0x07230203u) return NULL;
    uint32_t *out = malloc((nw + 64) * sizeof *out); if (!out) return NULL;
    size_t o = 0; memcpy(out, in, 5 * sizeof *out); o = 5;
    for (size_t i = 5; i < nw;) {
        uint32_t w = in[i], op = w & 0xffff, wc = w >> 16;
        if (!wc || i + wc > nw) { free(out); return NULL; }
        memcpy(out + o, in + i, wc * sizeof *out); o += wc;
        if (op == 72 && wc == 5 && in[i + 3] == 11 && in[i + 4] == 0 && o + 4 <= nw + 64) { out[o++] = 4u << 16 | 72u; out[o++] = in[i + 1]; out[o++] = in[i + 2]; out[o++] = 18; }   // OpMemberDecorate %s n Invariant
        else if (op == 71 && wc == 4 && in[i + 2] == 11 && in[i + 3] == 0 && o + 3 <= nw + 64) { out[o++] = 3u << 16 | 71u; out[o++] = in[i + 1]; out[o++] = 18; }                          // OpDecorate %v Invariant
        i += wc;
    }
    *outbytes = o * sizeof *out;
    return out;
}
VkShaderModule nvn_shader_module(nvn_shader *s) {
    pthread_mutex_lock(&g_mu);
    if (!s->module) {
        vkctx_t *c = nvn_vk_ctx();
        size_t pb = s->bytes; uint32_t *patched = NULL;
        static int plain = -1; if (plain < 0) plain = getenv("NX_NO_INVARIANT") != NULL;     // NX_NO_INVARIANT=1: the translator's SPIR-V as it is
        if (s->stage == NVN_STAGE_VERTEX && !plain) patched = spirv_invariant_position(s->spirv, s->bytes, &pb);
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = pb, .pCode = patched ? patched : s->spirv};
        if (!c || vkCreateShaderModule(c->dev, &ci, NULL, &s->module)) s->module = VK_NULL_HANDLE;
        free(patched);
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
