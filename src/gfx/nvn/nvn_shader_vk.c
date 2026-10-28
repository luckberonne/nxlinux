#include "nvn_shader_vk.h"
#include "nvn_vk_priv.h"
#include "nvn_draw.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct nvn_shader {
    uint8_t key[8];
    int stage;
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
    memcpy(s->key, key, 8); s->stage = d->stage;
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
