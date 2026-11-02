#include "ipc.h"
#include "kernel.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define S_H0 0xE0000u              // session handles: outside the kernel's table (0x1000 + slot)
#define S_MAX 256
#define R_NOT_REGISTERED 0xE15u    // sm ResultNotRegistered
#define R_NOT_IMPL 0xF201u

// A session may be converted to a domain: its requests then carry an object id, and the objects the service hands out are ids in the same domain instead of new sessions.
#define DOM_MAX 1024
typedef struct { uint32_t id; char name[16]; ipc_handler_t fn; void *data; } dom_obj_t;
// The objects belong to the domain, not to the session: every clone of a session sees the same table, and it goes with the last of them (refs).
struct domain { uint32_t next_id; int refs; dom_obj_t obj[DOM_MAX]; };
static pthread_mutex_t g_dom_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { int used; char name[16]; ipc_handler_t fn; void *data; struct domain *dom; } g_s[S_MAX];
static struct { char name[16]; ipc_handler_t fn; } g_svc[64];
static int g_nsvc;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_trace = -1;

// State made with ipc_new_session_owned: each session and domain object holding it is a reference, and the last one to close calls fin
#define OWN_MAX 1024
static struct { void *data; void (*fin)(void *); int refs; } g_own[OWN_MAX];
static pthread_mutex_t g_own_mu = PTHREAD_MUTEX_INITIALIZER;
static void own_ref(void *data) {
    if (!data) return;
    pthread_mutex_lock(&g_own_mu);
    for (int i = 0; i < OWN_MAX; i++) if (g_own[i].data == data) { g_own[i].refs++; break; }
    pthread_mutex_unlock(&g_own_mu);
}
static void own_unref(void *data) {
    if (!data) return;
    void (*fin)(void *) = NULL;
    pthread_mutex_lock(&g_own_mu);
    for (int i = 0; i < OWN_MAX; i++) if (g_own[i].data == data) { if (--g_own[i].refs == 0) { fin = g_own[i].fin; g_own[i].data = NULL; } break; }
    pthread_mutex_unlock(&g_own_mu);
    if (fin) fin(data);
}

void ipc_register_service(const char *name, ipc_handler_t fn) {
    pthread_mutex_lock(&g_mu);
    if (g_nsvc == 64) { fprintf(stderr, "[RED] ipc: service table full\n"); abort(); }
    strlcpy(g_svc[g_nsvc].name, name, 16); g_svc[g_nsvc++].fn = fn;
    pthread_mutex_unlock(&g_mu);
}

uint32_t ipc_new_session_data(const char *name, ipc_handler_t fn, void *data) {
    uint32_t h = ipc_new_session(name, fn);
    g_s[h - S_H0].data = data;
    return h;
}

uint32_t ipc_new_session_owned(const char *name, ipc_handler_t fn, void *data, void (*fin)(void *)) {
    pthread_mutex_lock(&g_own_mu);
    int i = 0; while (i < OWN_MAX && g_own[i].data) i++;
    if (i < OWN_MAX) { g_own[i].data = data; g_own[i].fin = fin; g_own[i].refs = 1; }
    pthread_mutex_unlock(&g_own_mu);
    if (i == OWN_MAX) fprintf(stderr, "[RED] ipc: more than %d objects with state to free: %s keeps its state\n", OWN_MAX, name);
    return ipc_new_session_data(name, fn, data);
}

uint32_t ipc_new_session(const char *name, ipc_handler_t fn) {
    pthread_mutex_lock(&g_mu);
    unsigned i = 0; while (i < S_MAX && g_s[i].used) i++;
    if (i == S_MAX) { fprintf(stderr, "[RED] ipc: more than %d sessions\n", S_MAX); abort(); }
    g_s[i].used = 1; strlcpy(g_s[i].name, name, sizeof g_s[i].name); g_s[i].fn = fn; g_s[i].data = NULL; g_s[i].dom = NULL;
    pthread_mutex_unlock(&g_mu);
    return S_H0 + i;
}

static int slot(uint32_t h) { unsigned i = h - S_H0; return i < S_MAX && g_s[i].used ? (int)i : -1; }

// ---- sm:
static uint32_t sm_handler(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: return 0;                                         // Initialize(pid)
    case 1: {                                                 // GetService(name[8]) → session handle
        char name[9] = {0}; memcpy(name, in->args, 8);
        pthread_mutex_lock(&g_mu);
        ipc_handler_t fn = NULL;
        for (int i = 0; i < g_nsvc; i++) if (!strncmp(g_svc[i].name, name, 8)) fn = g_svc[i].fn;
        pthread_mutex_unlock(&g_mu);
        if (!fn) { fprintf(stderr, "[RED] ipc: sm GetService \"%s\": the host does not serve it\n", name); return R_NOT_REGISTERED; }
        out->move[out->nmove++] = ipc_new_session(name, fn);
        return 0;
    }
    default: fprintf(stderr, "[RED] ipc: sm command %u is not implemented\n", in->cmd); return R_NOT_IMPL;
    }
}

// ---- messages
// Request: {u32 type | counts, u32 size | special flag, [special header: u32 flags, pid, handles], descriptors, raw data at the next 16-byte boundary of the message}
static int parse(const uint32_t *m, ipc_in_t *in, uint32_t *type, const uint32_t **raw_out, unsigned *words_out) {
    uint32_t w0 = m[0], w1 = m[1];
    *type = w0 & 0xffff;
    unsigned nx = w0 >> 16 & 15, na = w0 >> 20 & 15, nb = w0 >> 24 & 15, nw = w0 >> 28, idx = 2;
    memset(in, 0, sizeof *in);
    if (w1 >> 31) {
        uint32_t sp = m[idx++];
        if (sp & 1) { in->send_pid = 1; idx += 2; }
        in->ncopy = sp >> 1 & 15; in->nmove = sp >> 5 & 15;
        if (in->ncopy > 8 || in->nmove > 8) return -1;
        for (unsigned i = 0; i < in->ncopy; i++) in->copy[i] = m[idx++];
        for (unsigned i = 0; i < in->nmove; i++) in->move[i] = m[idx++];
    }
    for (unsigned i = 0; i < nx; i++, idx += 2) if (in->nsend < 4) {        // X (pointer) descriptor: {counter, address bits, size << 16; address}
        in->send[in->nsend].size = m[idx] >> 16; in->send[in->nsend++].addr = m[idx + 1] | (cpu_is_aarch32() ? 0 : (uint64_t)(m[idx] >> 12 & 15) << 32 | (uint64_t)(m[idx] >> 6 & 7) << 36);    // a 64-bit process: address bits 32-35 in bits 12-15, 36-38 in bits 6-8
    }
    for (unsigned i = 0; i < na + nb + nw; i++, idx += 3) {                // A (send), B (receive), W (exchange) descriptor: {size; address; flags, address and size high bits}
        ipc_buf_t b = {m[idx + 1] | (cpu_is_aarch32() ? 0 : (uint64_t)(m[idx + 2] >> 28) << 32 | (uint64_t)(m[idx + 2] >> 2 & 7) << 36), m[idx]};    // 64-bit: address bits 32-35 in word 2 bits 28-31, 36-38 in bits 2-4
        if (i < na || i >= na + nb) { if (in->nsend < 4) in->send[in->nsend++] = b; } else if (in->nrecv < 4) in->recv[in->nrecv++] = b;
    }
    const uint32_t *raw = m + (((idx * 4) + 15) & ~15u) / 4;
    unsigned words = w1 & 0x3ff;
    in->control = *type == 5 || *type == 7;
    // The receive list (C descriptors): w1 bits 10-13 say how many (0 none, 2 one, 3.. = mode - 2), bits 20-30 where it starts in the message. Each entry is {address; size << 16 | address bits}
    unsigned mode = w1 >> 10 & 15, loff = w1 >> 20 & 0x7ff, nc = mode == 2 ? 1 : mode > 2 ? mode - 2 : 0;
    const uint32_t *list = loff ? m + loff : raw + words - 2 * nc;      // offset 0: the list ends the data words, which count it
    if (!loff && nc) { if (words < 4 + 2 * nc) return -1; words -= 2 * nc; }
    for (unsigned i = 0; i < nc; i++) if (in->nrecv < 4) { ipc_buf_t b = {list[2 * i] | (cpu_is_aarch32() ? 0 : (uint64_t)(list[2 * i + 1] & 0xFFFF) << 32), list[2 * i + 1] >> 16}; in->recv[in->nrecv++] = b; }
    *raw_out = raw; *words_out = words;
    return words < 4 ? -1 : 0;
}

static void reply(uint32_t *m, const ipc_out_t *out, const uint32_t *objs, unsigned nobjs, int domain) {
    unsigned idx = 2, special = out->ncopy || (!domain && out->nmove);
    m[0] = 0;
    if (special) {
        m[idx++] = out->ncopy << 1 | out->nmove << 5;
        for (unsigned i = 0; i < out->ncopy; i++) m[idx++] = out->copy[i];
        if (!domain) for (unsigned i = 0; i < out->nmove; i++) m[idx++] = out->move[i];
    }
    uint32_t *raw = m + (((idx * 4) + 15) & ~15u) / 4, n = 0;
    if (domain) { raw[0] = nobjs; raw[1] = raw[2] = raw[3] = 0; n = 4; }            // DomainOutHeader {u32 objects, 12 bytes}
    raw[n] = 0x4f434653; raw[n + 1] = 0; raw[n + 2] = out->result; raw[n + 3] = 0;   // "SFCO", version 0, Result, token
    memcpy(raw + n + 4, out->out, out->nout * 4);
    if (domain) memcpy(raw + n + 4 + out->nout, objs, nobjs * 4);                  // the ids of the objects the reply creates follow the data
    m[1] = (n + 4 + out->nout + nobjs) | (special ? 1u << 31 : 0);
}

static uint32_t send(uint32_t h, uint32_t *m) {
    int s = slot(h);
    if (s < 0) return 0xE401u;                                       // invalid handle
    ipc_in_t in; ipc_out_t out; uint32_t type;
    memset(&out, 0, sizeof out);
    if ((m[0] & 0xffff) == 2) return 0;                                // Close: a message with no data; the handle itself goes with svcCloseHandle
    const uint32_t *raw; unsigned words;
    if (parse(m, &in, &type, &raw, &words)) { fprintf(stderr, "[RED] ipc: malformed message to %s\n", g_s[s].name); return 0xF601u; }
    if (g_trace < 0) g_trace = getenv("NX_IPC_TRACE") != NULL;
    ipc_handler_t fn = g_s[s].fn; const char *oname = g_s[s].name; in.data = g_s[s].data;
    uint32_t objs[8]; unsigned nobjs = 0; struct domain *D = g_s[s].dom; int dom = D != NULL && !in.control;
    if (dom) {                                                         // DomainInHeader {u8 type, u8 in objects, u16 data size, u32 object id, u32, u32 token}
        unsigned dtype = raw[0] & 0xff, dsize = raw[0] >> 16; uint32_t id = raw[1];
        pthread_mutex_lock(&g_dom_mu);
        dom_obj_t *o = NULL; for (int k = 0; k < DOM_MAX; k++) if (D->obj[k].id == id) o = &D->obj[k];
        dom_obj_t found; if (o) found = *o;
        if (o && dtype == 2) o->id = 0;                                   // close the object
        pthread_mutex_unlock(&g_dom_mu);
        if (!o) { fprintf(stderr, "[RED] ipc: %s: no object %u in the domain\n", g_s[s].name, id); out.result = 0xE401u; reply(m, &out, objs, 0, 1); return 0; }
        if (dtype == 2) { own_unref(found.data); reply(m, &out, objs, 0, 1); return 0; }
        fn = found.fn; oname = found.name; in.data = found.data;
        const uint32_t *c = raw + 4;                                   // SFCI header, then the arguments (dsize counts both)
        in.cmd = c[2]; in.nargs = dsize / 4 < 4 ? 0 : dsize / 4 - 4; if (in.nargs > IPC_MAX_ARGS) in.nargs = IPC_MAX_ARGS;
        memcpy(in.args, c + 4, in.nargs * 4);
    } else {
        in.cmd = raw[2]; in.nargs = words - 4 > IPC_MAX_ARGS ? IPC_MAX_ARGS : words - 4;
        memcpy(in.args, raw + 4, in.nargs * 4);
    }
    if (g_trace) {
        if (getenv("NX_IPC_RAW")) { fprintf(stderr, "[ipc]   raw:"); for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", m[i]); fprintf(stderr, "\n"); }
        fprintf(stderr, "[ipc] %s%s cmd %u type %u args:", oname, in.control ? " control" : "", in.cmd, type);
        for (unsigned i = 0; i < in.nargs && i < 8; i++) fprintf(stderr, " %08x", in.args[i]);
        for (unsigned i = 0; i < in.nrecv; i++) fprintf(stderr, " [out %#llx+%#x]", (unsigned long long)in.recv[i].addr, in.recv[i].size);       // where a reply buffer lands
        fprintf(stderr, "\n");
    }
    if (in.control) {
        switch (in.cmd) {
        case 0:                                                        // ConvertCurrentObjectToDomain → the object's id
            D = g_s[s].dom = calloc(1, sizeof *D); D->refs = 1;
            own_ref(g_s[s].data);                                      // object 1 holds the session's state as well as the session
            D->obj[0].id = 1; strlcpy(D->obj[0].name, g_s[s].name, 16); D->obj[0].fn = g_s[s].fn; D->obj[0].data = g_s[s].data; D->next_id = 2;
            out.out[out.nout++] = 1; break;
        case 2: case 4: {                                              // CloneCurrentObject(Ex): another session to the same object, in the same domain if there is one
            uint32_t h2 = ipc_new_session_data(g_s[s].name, g_s[s].fn, g_s[s].data); int c2 = slot(h2);
            own_ref(g_s[s].data);
            g_s[c2].dom = g_s[s].dom;                                 // the same domain
            if (g_s[c2].dom) { pthread_mutex_lock(&g_dom_mu); g_s[c2].dom->refs++; pthread_mutex_unlock(&g_dom_mu); }
            out.move[out.nmove++] = h2; break;                                 // the clone comes back as a move handle
        }
        case 3: out.out[out.nout++] = 0x1000; break;                                              // QueryPointerBufferSize: a service whose arrays travel by pointer (X) needs room for them
        default: fprintf(stderr, "[RED] ipc: control command %u on %s is not implemented\n", in.cmd, g_s[s].name); out.result = R_NOT_IMPL; break;
        }
    } else { kernel_note_ipc(oname, in.cmd); out.result = fn ? fn(s, &in, &out) : 0xE401u; kernel_note_ipc(NULL, 0); }
    if (dom) {                                                         // the sessions the handler made become objects of this domain
        for (unsigned k = 0; k < out.nmove && nobjs < 8; k++) {
            int ns = slot(out.move[k]); if (ns < 0) continue;
            pthread_mutex_lock(&g_dom_mu);
            for (int q = 0; q < DOM_MAX; q++) if (!D->obj[q].id) {
                D->obj[q].id = D->next_id++; strlcpy(D->obj[q].name, g_s[ns].name, 16); D->obj[q].fn = g_s[ns].fn; D->obj[q].data = g_s[ns].data;
                objs[nobjs++] = D->obj[q].id; break;
            }
            pthread_mutex_unlock(&g_dom_mu);
            g_s[ns].used = 0;
        }
    }
    reply(m, &out, objs, nobjs, dom);
    if (g_trace) { fprintf(stderr, "[ipc]   reply:"); for (int i = 0; i < 12; i++) fprintf(stderr, " %08x", m[i]); fprintf(stderr, "\n"); }
    return 0;                                                          // the SVC itself succeeded; the service's Result is inside the reply
}

int ipc_svc(cpu_t *c, uint32_t swi) {
    switch (swi) {
    case 0x1f: {   // ConnectToNamedPort(→w1 session; x1 name)
        const char *name = (const char *)cpu_get_x(c, 1);
        if (strncmp(name, "sm:", 4)) { fprintf(stderr, "[RED] ipc: ConnectToNamedPort \"%.12s\": no such port\n", name); cpu_set_x(c, 0, 0xF201u); return 1; }
        cpu_set_x(c, 0, 0); cpu_set_x(c, 1, ipc_new_session("sm:", sm_handler)); return 1;
    }
    case 0x15:     // CreateTransferMemory(→w1 handle; x1 address, x2 size, w3 permission): the memory stays where it is, the handle is what a service is given
        cpu_set_x(c, 0, 0); cpu_set_x(c, 1, ipc_new_session("tmem", NULL)); return 1;
    case 0x13: {   // MapSharedMemory(w0 handle, x1 address, x2 size, w3 permission): the memory the guest picked becomes accessible, zeroed (the host owns no shared contents: input and the like read as idle)
        uint64_t addr = cpu_get_x(c, 1), size = cpu_get_x(c, 2);
        if (slot((uint32_t)cpu_get_x(c, 0)) < 0) return 0;
        mprotect((void *)addr, size, PROT_READ | PROT_WRITE); memset((void *)addr, 0, size);
        ipc_shmem_t *blob = g_s[slot((uint32_t)cpu_get_x(c, 0))].data;             // a block with contents of its own (the shared fonts): copied in
        if (blob && blob->data) memcpy((void *)addr, blob->data, blob->size < size ? blob->size : size);
        if (blob) __atomic_store_n(&blob->mapped, addr, __ATOMIC_RELEASE);
        cpu_set_x(c, 0, 0); return 1;
    }
    case 0x5f: cpu_set_x(c, 0, 0); return 1;                         // FlushEntireDataCache: x86 caches are coherent, nothing to do
    case 0x14: {   // UnmapSharedMemory
        int s = slot((uint32_t)cpu_get_x(c, 0)); if (s < 0) return 0;
        ipc_shmem_t *blob = g_s[s].data; if (blob) __atomic_store_n(&blob->mapped, 0, __ATOMIC_RELEASE);
        cpu_set_x(c, 0, 0); return 1;
    }
    case 0x21: cpu_set_x(c, 0, send((uint32_t)cpu_get_x(c, 0), (uint32_t *)kernel_tls())); return 1;    // SendSyncRequest(handle)
    case 0x22: cpu_set_x(c, 0, send((uint32_t)cpu_get_x(c, 2), (uint32_t *)cpu_get_x(c, 0))); return 1;  // SendSyncRequestWithUserBuffer(buffer, size, handle)
    case 0x16: {   // CloseHandle of a session
        uint32_t h = (uint32_t)cpu_get_x(c, 0); int s = slot(h);
        if (s < 0) return 0;
        pthread_mutex_lock(&g_mu); void *data = g_s[s].data; struct domain *D = g_s[s].dom; g_s[s].used = 0; g_s[s].data = NULL; g_s[s].dom = NULL; pthread_mutex_unlock(&g_mu);
        own_unref(data);
        if (D) {                                                       // the last session of a domain closes its objects
            pthread_mutex_lock(&g_dom_mu); int last = --D->refs == 0; pthread_mutex_unlock(&g_dom_mu);
            if (last) { for (int k = 0; k < DOM_MAX; k++) if (D->obj[k].id) own_unref(D->obj[k].data); free(D); }
        }
        cpu_set_x(c, 0, 0); return 1;
    }
    }
    return 0;
}
