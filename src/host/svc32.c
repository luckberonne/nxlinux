#include "svc32.h"
#include <stdlib.h>
#include <string.h>

// One SVC. in[k]: the handler's x register k gets guest register lo (and hi << 32 when hi >= 0). out[k]: the handler's x register k goes to lo (and its high word to hi).
// A register of -1 ends the list. Pointers and 32-bit values are zero-extended: the handlers cast the ones that are signed.
typedef struct { int8_t x, lo, hi; } map_t;
typedef struct { uint8_t num; map_t in[6], out[4]; } svc_t;

#define END {-1, -1, -1}
// Result in r0 for everything that returns one
static const svc_t T[] = {
    {0x01, {{1, 1, -1}, END},                                            {{0, 0, -1}, {1, 1, -1}, END}},            // SetHeapSize(→r1 base; r1 size)
    {0x02, {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, END},                    {{0, 0, -1}, END}},                        // SetMemoryPermission(addr, size, perm)
    {0x03, {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, {3, 3, -1}, END},        {{0, 0, -1}, END}},                        // SetMemoryAttribute(addr, size, mask, attr)
    {0x04, {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, END},                    {{0, 0, -1}, END}},                        // MapMemory(dst, src, size)
    {0x05, {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, END},                    {{0, 0, -1}, END}},                        // UnmapMemory(dst, src, size)
    {0x06, {{0, 0, -1}, {2, 2, -1}, END},                                {{0, 0, -1}, {1, 1, -1}, END}},            // QueryMemory(MemoryInfo* r0, →r1 PageInfo; r2 address)
    {0x08, {{1, 1, -1}, {2, 2, -1}, {3, 3, -1}, {4, 0, -1}, {5, 4, -1}, END},
                                                                         {{0, 0, -1}, {1, 1, -1}, END}},            // CreateThread(→r1 handle; r1 entry, r2 arg, r3 stack top, r0 priority, r4 core)
    {0x09, {{0, 0, -1}, END},                                            {{0, 0, -1}, END}},                        // StartThread(handle)
    {0x0B, {{0, 0, 1}, END},                                             {{-1, -1, -1}}},                           // SleepThread(ns in r0:r1)
    {0x0C, {{1, 1, -1}, END},                                            {{0, 0, -1}, {1, 1, -1}, END}},            // GetThreadPriority(→r1 priority; r1 handle)
    {0x0D, {{0, 0, -1}, {1, 1, -1}, END},                                {{0, 0, -1}, END}},                        // SetThreadPriority(handle, priority)
    {0x0E, {{2, 2, -1}, END},                                            {{0, 0, -1}, {1, 1, -1}, {2, 2, 3}, END}}, // GetThreadCoreMask(→r1 ideal core, →r2:r3 mask; r2 handle)
    {0x0F, {{0, 0, -1}, {1, 1, -1}, {2, 2, 3}, END},                     {{0, 0, -1}, END}},                        // SetThreadCoreMask(handle, ideal core, mask in r2:r3)
    {0x10, {END},                                                        {{0, 0, -1}, END}},                        // GetCurrentProcessorNumber → r0
    {0x11, {{0, 0, -1}, END},                                            {{0, 0, -1}, END}},                        // SignalEvent(handle)
    {0x12, {{0, 0, -1}, END},                                            {{0, 0, -1}, END}},                        // ClearEvent(handle)
    {0x15, {{1, 1, -1}, {2, 2, -1}, {3, 3, -1}, END},                    {{0, 0, -1}, {1, 1, -1}, END}},            // CreateTransferMemory(→r1 handle; r1 address, r2 size, r3 permission)
    {0x13, {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, {3, 3, -1}, END},        {{0, 0, -1}, END}},                        // MapSharedMemory(handle, address, size, permission)
    {0x14, {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, END},                    {{0, 0, -1}, END}},                        // UnmapSharedMemory(handle, address, size)
    {0x5f, {END},                                                        {{0, 0, -1}, END}},                        // FlushEntireDataCache
    {0x16, {{0, 0, -1}, END},                                            {{0, 0, -1}, END}},                        // CloseHandle(handle)
    {0x17, {{0, 0, -1}, END},                                            {{0, 0, -1}, END}},                        // ResetSignal(handle)
    {0x18, {{1, 1, -1}, {2, 2, -1}, {3, 0, 3}, END},                     {{0, 0, -1}, {1, 1, -1}, END}},            // WaitSynchronization(→r1 index; r1 handles, r2 count, timeout in r0:r3)
    {0x19, {{0, 0, -1}, END},                                            {{0, 0, -1}, END}},                        // CancelSynchronization(thread)
    {0x1A, {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, END},                    {{0, 0, -1}, END}},                        // ArbitrateLock(owner, address, tag)
    {0x1B, {{0, 0, -1}, END},                                            {{0, 0, -1}, END}},                        // ArbitrateUnlock(address)
    {0x1C, {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, {3, 3, 4}, END},         {{0, 0, -1}, END}},                        // WaitProcessWideKeyAtomic(address, key, tag, timeout in r3:r4)
    {0x1D, {{0, 0, -1}, {1, 1, -1}, END},                                {{0, 0, -1}, END}},                        // SignalProcessWideKey(key, count)
    {0x1E, {{-1, -1, -1}},                                               {{0, 0, 1}, END}},                         // GetSystemTick → r0:r1
    {0x1f, {{1, 1, -1}, END},                                            {{0, 0, -1}, {1, 1, -1}, END}},            // ConnectToNamedPort(→r1 session; r1 name)
    {0x21, {{0, 0, -1}, END},                                            {{0, 0, -1}, END}},                        // SendSyncRequest(session)
    {0x22, {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, END},                    {{0, 0, -1}, END}},                        // SendSyncRequestWithUserBuffer(buffer, size, session)
    {0x25, {{1, 1, -1}, END},                                            {{0, 0, -1}, {1, 1, 2}, END}},             // GetThreadId(→r1:r2 id; r1 handle)
    {0x26, {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, END},                    {{0, 0, -1}, END}},                        // Break(reason, address, size)
    {0x27, {{0, 0, -1}, {1, 1, -1}, END},                                {{0, 0, -1}, END}},                        // OutputDebugString(string, size)
    {0x29, {{1, 1, -1}, {2, 2, -1}, {3, 0, 3}, END},                     {{0, 0, -1}, {1, 1, 2}, END}},             // GetInfo(→r1:r2 value; r1 type, r2 handle, subtype in r0:r3)
    {0x2C, {{0, 0, -1}, {1, 1, -1}, END},                                {{0, 0, -1}, END}},                        // MapPhysicalMemory(address, size)
    {0x2D, {{0, 0, -1}, {1, 1, -1}, END},                                {{0, 0, -1}, END}},                        // UnmapPhysicalMemory(address, size)
    {0x34, {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, {3, 3, 4}, END},         {{0, 0, -1}, END}},                        // WaitForAddress(address, type, value, timeout in r3:r4)
    {0x35, {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, {3, 3, -1}, END},        {{0, 0, -1}, END}},                        // SignalToAddress(address, type, value, count)
    {0x45, {END},                                                        {{0, 0, -1}, {1, 1, -1}, {2, 2, -1}, END}},// CreateEvent(→r1 write end, →r2 read end)
};

static const svc_t *find(uint32_t num) {
    for (unsigned i = 0; i < sizeof T / sizeof *T; i++) if (T[i].num == num) return &T[i];
    return NULL;
}

// ---- hooks: the parameters of a guest function, from its Itanium mangled name
#define HOOK_BASE 0x1000u
#define HOOK_MAX 8192
enum { K_WORD, K_WIDE, K_FLOAT, K_DOUBLE };                         // a parameter: one core word, a 64-bit core pair, an s register, a d register (hard-float)
enum { R_WORD, R_WIDE, R_FLOAT, R_DOUBLE, R_UNKNOWN };                // what a hook returns; R_UNKNOWN (the return type is not in a mangled name) returns r0 and r1
typedef struct { uint8_t n; uint8_t wide[8]; uint8_t ret; uint8_t sret; } sig_t;   // wide[]: the K_ kind of each parameter
static sig_t *g_hook[HOOK_MAX];
static const sig_t g_default_sig = {8, {0}, R_UNKNOWN};       // a stub nobody registered a name for (nvn function pointers handed out at run time): eight one-word parameters
extern const char *hle_stub_name(uint32_t swi) __attribute__((weak));
static const struct { const char *name, *sig; char ret; } g_nvn_sig[] = {
#include "nvn_sig32.inc"
};
static void register_sig(uint32_t swi, const char *params, char ret);
// A stub nobody gave a mangled name: the NVN functions the host hands out (named "nvn:<function>") have their signature from nvn.h; anything else is eight one-word parameters.
static const sig_t *hook_sig(uint32_t num) {
    if (num < HOOK_BASE || num >= HOOK_BASE + HOOK_MAX) return NULL;
    if (!g_hook[num - HOOK_BASE]) {
        const char *nm = hle_stub_name ? hle_stub_name(num) : NULL;
        if (nm && !strncmp(nm, "nvn:", 4)) {
            for (unsigned i = 0; i < sizeof g_nvn_sig / sizeof *g_nvn_sig; i++)
                if (!strcmp(g_nvn_sig[i].name, nm + 4)) { register_sig(num, g_nvn_sig[i].sig, g_nvn_sig[i].ret); break; }
        }
    }
    return g_hook[num - HOOK_BASE] ? g_hook[num - HOOK_BASE] : &g_default_sig;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
// Skip a <source-name> (length-prefixed identifier) at *p.
static void skip_id(const char **p) { int n = 0; while (is_digit(**p)) n = n * 10 + *(*p)++ - '0'; for (; n && **p; n--) (*p)++; }
// Skip a balanced construct that opened with one of N, I, L, X (at *p, already consumed) up to its E. Identifiers and literals are skipped whole, so letters inside them do not count.
static void skip_to_e(const char **p, int depth) {
    while (**p && depth) {
        char c = *(*p)++;
        if (is_digit(c)) { (*p)--; skip_id(p); }
        else if (c == 'N' || c == 'I' || c == 'X' || c == 'F') depth++;
        else if (c == 'L') { while (**p && **p != 'E') (*p)++; if (**p) (*p)++; }    // a literal: L<type><value>E
        else if (c == 'E') depth--;
    }
}
// One <type> at *p; returns its K_ kind (a pointer or reference is one word whatever it points at), -1 if the text ended or is not a type.
static int skip_type(const char **p) {
    char c = *(*p)++;
    switch (c) {
    case 0: return -1;
    case 'v': case 'w': case 'b': case 'c': case 'a': case 'h': case 's': case 't': case 'i': case 'j': case 'l': case 'm': case 'n': case 'o': case 'e': case 'g': return K_WORD;
    case 'f': return K_FLOAT;
    case 'x': case 'y': return K_WIDE;
    case 'd': return K_DOUBLE;
    case 'z': return 0;
    case 'T': { while (**p && **p != '_') (*p)++; if (**p) (*p)++; return 0; }          // a template parameter (T_, T0_)
    case 'P': case 'R': case 'O': { int r = skip_type(p); return r < 0 ? -1 : 0; }           // a pointer or reference: one word whatever it points at
    case 'K': case 'V': case 'r': return skip_type(p);
    case 'N': skip_to_e(p, 1); return 0;
    case 'F': skip_to_e(p, 1); return 0;
    case 'A': { while (**p && **p != '_') (*p)++; if (**p) (*p)++; return skip_type(p) < 0 ? -1 : 0; }   // an array parameter is a pointer
    case 'M': { if (skip_type(p) < 0 || skip_type(p) < 0) return -1; return 0; }
    case 'S': {
        if (**p == 't') { (*p)++; skip_id(p); }
        else if (**p == 'a' || **p == 'b' || **p == 's' || **p == 'i' || **p == 'o' || **p == 'd') (*p)++;
        else { while (**p && **p != '_') (*p)++; if (**p) (*p)++; }
        if (**p == 'I') { (*p)++; skip_to_e(p, 1); }
        return 0;
    }
    default:
        if (is_digit(c)) { (*p)--; skip_id(p); if (**p == 'I') { (*p)++; skip_to_e(p, 1); } return 0; }
        return -1;
    }
}

// Parses the parameter list of _Z<encoding>; returns the count, -1 on failure.
static int parse_sig(const char *m, sig_t *sig) {
    memset(sig, 0, sizeof *sig); sig->ret = R_UNKNOWN;
    if (strncmp(m, "_Z", 2)) return -1;
    const char *p = m + 2;
    int templ = 0;
    if (*p == 'N') {                                          // nested name: qualifiers, components, maybe trailing template arguments
        p++;
        while (*p == 'K' || *p == 'V' || *p == 'r' || *p == 'R' || *p == 'O') p++;
        while (*p && *p != 'E') {
            templ = 0;
            if (is_digit(*p)) skip_id(&p);
            else if (*p == 'S') { p++; if (*p == 't') p++; else { while (*p && *p != '_') p++; if (*p) p++; } }
            else if (*p == 'I') { p++; skip_to_e(&p, 1); templ = 1; }
            else if (*p == 'C' || *p == 'D') p += 2;          // constructor / destructor
            else if (*p == 'L') p++;
            else {                                            // an operator name or similar: two letters
                p += 2;
            }
        }
        if (*p == 'E') p++;
    } else if (is_digit(*p)) { skip_id(&p); if (*p == 'I') { p++; skip_to_e(&p, 1); templ = 1; } }
    else if (*p == 'S' && p[1] == 't') { p += 2; skip_id(&p); if (*p == 'I') { p++; skip_to_e(&p, 1); templ = 1; } }
    else return -1;
    if (templ && skip_type(&p) < 0) return -1;                // a function template's encoding starts with its return type
    if (*p == 'v' && !p[1]) return 0;
    while (*p) {
        if (sig->n == 8) return sig->n;
        int w = skip_type(&p);
        if (w < 0) return -1;
        sig->wide[sig->n++] = (uint8_t)w;
    }
    return sig->n;
}

// Hooks whose result is a class of 8 bytes: AAPCS32 returns it through a pointer the caller passes in r0 (the other arguments move up one register), where the AArch64 host function
// returns a plain uint64_t. Only the hooks the host has are listed; their types are in the SDK headers: AppletResourceUserId, settings::LanguageCode, time::PosixTime and a time_point.
static const char *const g_sret_names[] = {
    "_ZN2nn6applet23GetAppletResourceUserIdEv", "_ZN2nn2oe18GetDesiredLanguageEv",
    "_ZN2nn4time23StandardUserSystemClock3nowEv", "_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE",
};

int svc32_register_hook(uint32_t swi, const char *mangled) {
    if (swi < HOOK_BASE || swi >= HOOK_BASE + HOOK_MAX) return -1;
    sig_t *sig = calloc(1, sizeof *sig);
    int n = mangled ? parse_sig(mangled, sig) : -1;
    if (n < 0) { memset(sig, 0, sizeof *sig); sig->n = 8; sig->ret = R_UNKNOWN; }
    if (mangled) for (unsigned i = 0; i < sizeof g_sret_names / sizeof *g_sret_names; i++) if (!strcmp(mangled, g_sret_names[i])) sig->sret = 1;
    free(g_hook[swi - HOOK_BASE]);
    g_hook[swi - HOOK_BASE] = sig;
    return n;
}

static void register_sig(uint32_t swi, const char *params, char ret) {
    sig_t *sig = calloc(1, sizeof *sig);
    for (const char *p = params; *p && sig->n < 8; p++) sig->wide[sig->n++] = *p == 'x' ? K_WIDE : *p == 'f' ? K_FLOAT : *p == 'd' ? K_DOUBLE : K_WORD;
    sig->ret = ret == 'x' ? R_WIDE : ret == 'f' ? R_FLOAT : ret == 'd' ? R_DOUBLE : R_WORD;
    free(g_hook[swi - HOOK_BASE]);
    g_hook[swi - HOOK_BASE] = sig;
}
int svc32_register_sig(uint32_t swi, const char *params, char ret) {
    if (swi < HOOK_BASE || swi >= HOOK_BASE + HOOK_MAX) return -1;
    register_sig(swi, params, ret);
    return (int)g_hook[swi - HOOK_BASE]->n;
}

// The AAPCS32 call of a hook (hard-float, as the title is built): integers take r0..r3 and then the stack, a 64-bit integer an even register pair (or 8-aligned on the stack); floats and
// doubles take the VFP registers s0.. / d0.., the lowest free ones. The host function reads them as AAPCS64 would pass them: integers in x0.. in order, floats and doubles in v0.. in order.
static void hook_enter(const sig_t *sig, const uint32_t r[16], const uint32_t *ext, uint64_t x[8], uint64_t fv[8]) {
    unsigned ncrn = sig->sret ? 1 : 0, nsaa = 0, ni = 0, nf = 0, used = 0;                   // used: the s registers taken
    const uint32_t *stack = (const uint32_t *)(uintptr_t)r[13];
    for (unsigned i = 0; i < sig->n && i < 8; i++) {
        switch (sig->wide[i]) {
        case K_FLOAT: case K_DOUBLE: {
            unsigned w = sig->wide[i] == K_DOUBLE ? 2 : 1, k = 0;
            for (; k + w <= 16; k += w) if (!(used >> k & ((1u << w) - 1))) break;
            if (k + w > 16 || !ext) { if (nf < 8) fv[nf++] = 0; break; }       // out of VFP registers (they would go on the stack): not supported
            used |= ((1u << w) - 1) << k;
            if (nf < 8) fv[nf++] = w == 2 ? ((uint64_t)ext[k + 1] << 32) | ext[k] : ext[k];
            break;
        }
        case K_WIDE:
            if (ncrn & 1) ncrn++;
            if (ni < 8) {
                if (ncrn <= 2) { x[ni++] = ((uint64_t)r[ncrn + 1] << 32) | r[ncrn]; ncrn += 2; }
                else { nsaa = (nsaa + 1) & ~1u; x[ni++] = ((uint64_t)stack[nsaa + 1] << 32) | stack[nsaa]; nsaa += 2; ncrn = 4; }
            }
            break;
        default:
            if (ni < 8) x[ni++] = ncrn < 4 ? r[ncrn++] : stack[nsaa++];
        }
    }
}

int svc32_known(uint32_t num) { return find(num) != NULL || hook_sig(num) != NULL; }

void svc32_enter(uint32_t num, const uint32_t r[16], uint64_t x[8], svc32_ctx_t *cx) { uint64_t fv[8]; svc32_enter2(num, r, NULL, x, fv, cx); }

void svc32_enter2(uint32_t num, const uint32_t r[16], const uint32_t *ext, uint64_t x[8], uint64_t fv[8], svc32_ctx_t *cx) {
    memset(x, 0, 8 * sizeof *x); memset(fv, 0, 8 * sizeof *fv);
    if (hook_sig(num)) { hook_enter(hook_sig(num), r, ext, x, fv); if (cx) cx->sret = hook_sig(num)->sret ? r[0] : 0; return; }
    const svc_t *s = find(num);
    if (!s) { for (int i = 0; i < 8; i++) x[i] = r[i]; return; }
    for (int k = 0; k < 6 && s->in[k].x >= 0; k++) {
        const map_t *m = &s->in[k];
        x[m->x] = m->hi >= 0 ? ((uint64_t)r[m->hi] << 32) | r[m->lo] : r[m->lo];
    }
    (void)cx;                                              // QueryMemory's MemoryInfo keeps its 64-bit fields in a 32-bit process, so the handler writes the guest's buffer itself
}

void svc32_leave(uint32_t num, const uint64_t x[8], uint32_t r[16], svc32_ctx_t *cx) { svc32_leave2(num, x, 0, r, NULL, cx); }

void svc32_leave2(uint32_t num, const uint64_t x[8], uint64_t fret, uint32_t r[16], uint32_t *ext, svc32_ctx_t *cx) {
    const sig_t *sig = hook_sig(num);
    if (sig) {
        if (sig->ret == R_FLOAT && ext) ext[0] = (uint32_t)fret;
        else if (sig->ret == R_DOUBLE && ext) { ext[0] = (uint32_t)fret; ext[1] = (uint32_t)(fret >> 32); }
        else if (sig->sret && cx && cx->sret) { memcpy((void *)(uintptr_t)cx->sret, &x[0], 8); r[0] = cx->sret; }   // the struct goes to the caller's buffer; r0 returns its address
        else { r[0] = (uint32_t)x[0]; if (sig->ret == R_WIDE || sig->ret == R_UNKNOWN) r[1] = (uint32_t)(x[0] >> 32); }
        return;
    }
    const svc_t *s = find(num);
    if (!s) { r[0] = (uint32_t)x[0]; return; }
    for (int k = 0; k < 4 && s->out[k].x >= 0; k++) {
        const map_t *m = &s->out[k];
        r[m->lo] = (uint32_t)x[m->x];
        if (m->hi >= 0) r[m->hi] = (uint32_t)(x[m->x] >> 32);
    }
}
