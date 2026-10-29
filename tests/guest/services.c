// Guest program for tests/test_services.cpp: calls the SDK's service client functions by their mangled names, as a game's code does. They are imports the host answers
// (src/host/svc/hle_*.c); the program also defines one of them itself, which a forced takeover has to override. Results go to g_res[].
typedef unsigned long u64; typedef long i64; typedef unsigned u32; typedef short i16; typedef signed char i8;
u64 g_res[32];

extern u32 _ZN2nn4time13IsInitializedEv(void);
extern u32 _ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE(i64 *out);
extern u32 _ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE(void *cal, void *info, const i64 *t);
extern u32 _ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE(unsigned char *out);
extern u32 _ZN2nn2sf4hipc31InitializeHipcServiceResolutionEv(void);
extern u32 _ZN2nn2sf4hipc20ConnectToHipcServiceEPNS_3svc6HandleEPKc(u32 *out, const char *name);
extern u32 _ZN2nn2sf4hipc24CloseClientSessionHandleENS_3svc6HandleE(u32 h);

// Defined here and also taken over by the host: the host's version must win (it returns 0, this one 99)
u32 _ZN2nn4time10InitializeEv(void) { return 99; }
u32 time_init_via_got(void) { return _ZN2nn4time10InitializeEv(); }

static void exit_thread(void) { register u64 x0 __asm__("x0"); __asm__ volatile("svc #0x0A" : "=r"(x0) : : "memory", "x1", "x2", "x3"); for (;;) {} }

void guest_main(u64 arg) {
    (void)arg;
    g_res[0] = time_init_via_got() == 0;                                   // the forced hook, not the definition above
    g_res[1] = _ZN2nn4time13IsInitializedEv() == 1;
    i64 t = 0;
    g_res[2] = _ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE(&t) == 0 && t > 1700000000 && t < 4000000000L;
    struct { i16 year; i8 month, day, hour, minute, second, pad; } cal; unsigned char info[24];
    g_res[3] = _ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE(&cal, info, &t) == 0 && cal.year >= 2025 && cal.month >= 1 && cal.month <= 12 && cal.day >= 1 && cal.day <= 31;
    unsigned char sp[24]; const char *want = "dmhost-steady-01"; int same = 1;
    g_res[4] = _ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE(sp) == 0;
    for (int i = 0; i < 16; i++) if (sp[8 + i] != (unsigned char)want[i]) same = 0;
    g_res[5] = same;
    u32 h = 0, h2 = 0;
    g_res[6] = _ZN2nn2sf4hipc31InitializeHipcServiceResolutionEv() == 0;
    g_res[7] = _ZN2nn2sf4hipc20ConnectToHipcServiceEPNS_3svc6HandleEPKc(&h, "olsc:u") == 0 && h != 0;
    g_res[8] = _ZN2nn2sf4hipc20ConnectToHipcServiceEPNS_3svc6HandleEPKc(&h2, "nope:u") != 0;       // a service the host does not serve is refused
    _ZN2nn2sf4hipc24CloseClientSessionHandleENS_3svc6HandleE(h);
    g_res[9] = 0x5555;
    exit_thread();
}
