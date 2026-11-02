// nn::diag: the game's own log lines. nn::diag::detail::LogImpl(meta, fmt, ...) formats the line and hands it to every log observer; the SDK's default observer (the lm
// service's) first asks for the process name by walking the module's memory with QueryMemory (GetModuleInfoForHorizon), and that walk ends in an assertion here, so a
// title that logs aborts the moment it does (Super Smash Bros. Ultimate does, early in its start). The host has no log service to feed: the line goes to the host's own log.
// The arguments are the ones in the registers (x2..x7: six of them, a title's log lines carry few); a %s is only read when it points into the guest window.
#include "hle.h"
#include "guest_mem.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int in_guest(uint64_t a, size_t n) { return a >= guest_mem_base() && a < guest_mem_end() && n <= guest_mem_end() - a; }

// vsnprintf for printf conversions over a list of 64-bit register values: %s %d %i %u %x %X %c %p %f-family (the register's bits as an integer: floats arrive in v0..v7, not here)
static void fmt_regs(char *out, size_t cap, const char *f, const uint64_t *args, int nargs) {
    size_t o = 0; int ai = 0;
    #define PUT(c) do { if (o + 1 < cap) out[o++] = (c); } while (0)
    for (; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        char spec[16]; int sn = 0; spec[sn++] = '%'; f++;
        while (*f && strchr("-+ #0123456789.*lhzjt", *f) && sn < 12) { if (*f == '*') { f++; continue; } spec[sn++] = *f++; }
        char c = *f; if (!c) break;
        if (c == '%') { PUT('%'); continue; }
        uint64_t v = ai < nargs ? args[ai] : 0; ai++;
        char piece[300]; piece[0] = 0;
        int is_long = 0; for (int i = 1; i < sn; i++) if (spec[i] == 'l' || spec[i] == 'z' || spec[i] == 'j' || spec[i] == 't') is_long = 1;
        if (c == 's') { if (v && in_guest(v, 1)) snprintf(piece, sizeof piece, "%.200s", (const char *)v); else snprintf(piece, sizeof piece, "(null)"); }
        else if (c == 'c') snprintf(piece, sizeof piece, "%c", (int)v);
        else if (c == 'p') snprintf(piece, sizeof piece, "%#llx", (unsigned long long)v);
        else if (strchr("di", c)) { if (is_long) snprintf(piece, sizeof piece, "%lld", (long long)v); else snprintf(piece, sizeof piece, "%d", (int)v); }      // width and precision are dropped: a log line stays readable
        else if (strchr("uxXo", c)) { if (is_long) snprintf(piece, sizeof piece, c == 'u' ? "%llu" : c == 'x' ? "%llx" : c == 'X' ? "%llX" : "%llo", (unsigned long long)v); else snprintf(piece, sizeof piece, c == 'u' ? "%u" : c == 'x' ? "%x" : c == 'X' ? "%X" : "%o", (unsigned)v); }
        else snprintf(piece, sizeof piece, "%%%c<%#llx>", c, (unsigned long long)v);     // a float or something unknown: shown as what it is
        for (const char *p = piece; *p; p++) PUT(*p);
    }
    #undef PUT
    out[o < cap ? o : cap - 1] = 0;
}

HLE_FORCE("_ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz", "the game's log line goes to the host log: the SDK's log observer aborts in the host (QueryMemory walk of the module)", void, h_diag_LogImpl,
          const void *meta, const char *fmt, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    (void)meta;
    if (!fmt || !in_guest((uint64_t)fmt, 1)) return;
    uint64_t args[6] = {a2, a3, a4, a5, a6, a7}; char line[1024];
    fmt_regs(line, sizeof line, fmt, args, 6);
    size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    static _Atomic unsigned count; unsigned k = atomic_fetch_add(&count, 1);
    if (k < 400 || !(k & (k - 1))) fprintf(stderr, "[game] %s\n", line);       // the first 400 lines, then one in a power of two: a title may log every frame
}
