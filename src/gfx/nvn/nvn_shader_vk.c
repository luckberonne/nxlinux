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
    VkShaderModule module;                 // made on first use
    struct nvn_shader *next;
} nvn_shader;

static nvn_shader *g_head;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

uint64_t nvn_vk_shader_key(const uint8_t key[8]) { uint64_t v; memcpy(&v, key, 8); return v; }

void nvn_vk_shader_register(const uint8_t key[8], const nvn_shader_desc *d) {
    nvn_shader *s = calloc(1, sizeof *s);
    memcpy(s->key, key, 8); s->stage = d->stage; s->needs = d->needs;
    s->spirv = malloc(d->bytes); memcpy(s->spirv, d->spirv, d->bytes); s->bytes = d->bytes;
    s->binds = malloc((d->nbinds ? d->nbinds : 1) * sizeof *s->binds); if (d->nbinds) memcpy(s->binds, d->binds, d->nbinds * sizeof *s->binds); s->nbinds = d->nbinds;
    pthread_mutex_lock(&g_mu);
    for (nvn_shader **p = &g_head; *p; p = &(*p)->next)
        if (!memcmp((*p)->key, key, 8)) { s->next = (*p)->next; /* the old one may still be in a pipeline: it is leaked on purpose */ *p = s; pthread_mutex_unlock(&g_mu); return; }
    s->next = g_head; g_head = s;
    pthread_mutex_unlock(&g_mu);
}

const nvn_shader *nvn_shader_find(const uint8_t key[8]) {
    pthread_mutex_lock(&g_mu);
    nvn_shader *s = g_head;
    while (s && memcmp(s->key, key, 8)) s = s->next;
    pthread_mutex_unlock(&g_mu);
    return s;
}
int nvn_shader_stage(const nvn_shader *s) { return s->stage; }
uint32_t nvn_shader_needs(const nvn_shader *s) { return s->needs; }
const nvn_shbind *nvn_shader_binds(const nvn_shader *s, int *n) { *n = s->nbinds; return s->binds; }
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
    if (ver != 2) { fprintf(stderr, "[dm] [RED] [nvn] shader library %s has version %u, expected 2\n", path, ver); goto done; }
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
        uint32_t *code = malloc(nbytes); memcpy(code, d + off, nbytes); off += nbytes;
        nvn_shader_desc desc = {(int)stage, needs, code, nbytes, binds, (int)nb};
        nvn_vk_shader_register(key, &desc);
        free(binds); free(code); count++;
    }
done:
    free(d);
    return count;
}
