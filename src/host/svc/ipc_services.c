// The services a title reaches through sm: (ipc.c), answered by the host. Each is a handler of CMIF requests: it reads in->cmd / in->args and fills out; session is the id of
// the session the request came on. A service is registered under the name the client gives sm. Commands are the ones switchbrew documents for the firmware the title targets;
// a command not listed is reported in red and answered with ResultNotImplemented, so the next one a title needs shows up in the log.
// A request that carries the caller's process id (send_pid) has two placeholder words at the start of in->args.
#include "ipc.h"
#include "kernel.h"
#include "hid_input.h"
#include "mem.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define R_NOT_IMPL 0xF201u
uint32_t fsfs_open_savedata(ipc_out_t *out);        // ipc_fs.c
#define R_NO_MESSAGE 0x680u        // applet ResultNoMessage: the message queue is empty
#define OUT32(v) (out->out[out->nout++] = (uint32_t)(v))
#define OUT64(v) do { uint64_t _v = (v); OUT32(_v); OUT32(_v >> 32); } while (0)

static uint32_t unimpl(const char *svc, const ipc_in_t *in) {
    fprintf(stderr, "[RED] ipc: %s command %u is not implemented\n", svc, in->cmd);
    return R_NOT_IMPL;
}
static uint32_t ok_all(const ipc_in_t *in, ipc_out_t *out, const char *svc) { (void)in; (void)out; (void)svc; return 0; }

// ---- lm: the logger. A title opens a logger and writes its log lines; nothing reads them here.
static uint32_t logger(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session; (void)out;
    switch (in->cmd) {
    case 0: return 0;     // Log(buffer)
    case 1: return 0;     // SetDestination(u32)
    default: return unimpl("lm logger", in);
    }
}
static uint32_t lm(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: out->move[out->nmove++] = ipc_new_session("lm.log", logger); return 0;   // OpenLogger(pid) → ILogger
    default: return unimpl("lm", in);
    }
}

// ---- apm: performance modes. The title runs handheld at the normal configuration.
static uint32_t apm_session(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: return 0;                          // SetPerformanceConfiguration(mode, configuration)
    case 1: OUT32(0x00010001); return 0;       // GetPerformanceConfiguration(mode) → configuration
    default: return unimpl("apm session", in);
    }
}
static uint32_t apm(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: out->move[out->nmove++] = ipc_new_session("apm.ses", apm_session); return 0;   // OpenSession → ISession
    case 1: OUT32(0); return 0;                // GetPerformanceMode: 0 = handheld (normal)
    default: return unimpl("apm", in);
    }
}

// ---- aoc:u: add-on content. None is installed.
static uint32_t aoc(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 2: OUT32(0); return 0;                                    // CountAddOnContent(pid) → 0
    case 3: OUT32(0); return 0;                                    // ListAddOnContent(pid, offset, count) → count written to the buffer: 0
    case 5: OUT64(mem_default_profile().program_id + 0x1000); return 0;   // GetAddOnContentBaseId(pid): the title id + 0x1000
    case 6: return 0;                                              // PrepareAddOnContent(index, pid)
    case 7: case 8: out->copy[out->ncopy++] = kernel_event_new(0); return 0;   // GetAddOnContentListChangedEvent → event (command 7 in the 1.0.0 SDK, 8 in the 4.x one)
    default: return unimpl("aoc:u", in);
    }
}

// ---- pctl: parental controls. Everything is unrestricted; only the commands a title uses are answered.
static uint32_t pctl_service(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 1: return 0;                                              // Initialize
    case 1001: return 0;                                           // CheckFreeCommunicationPermission
    case 1031: OUT32(0); return 0;                                 // IsRestrictionEnabled → false
    case 1032: OUT32(0); return 0;                                 // GetSafetyLevel
    case 1013: case 1002: case 1003: return 0;                     // ConfirmLaunchApplicationPermission and the playable-application checks
    default: return unimpl("pctl", in);
    }
}
static uint32_t pctl(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: case 1: out->move[out->nmove++] = ipc_new_session("pctl.svc", pctl_service); return 0;   // CreateService(pid) / CreateServiceWithoutInitialize(pid)
    default: return unimpl("pctl:*", in);
    }
}

// ---- appletOE: the application's view of the applet manager. OpenApplicationProxy hands out the proxy whose objects drive focus, messages and the window.
static uint32_t g_focus_event;        // ICommonStateGetter::GetEventHandle: signaled when a message is waiting
static int g_msg_queue[8], g_msg_n;   // applet messages the title has not received yet

static void applet_post(int msg) { if (g_msg_n < 8) { g_msg_queue[g_msg_n++] = msg; if (g_focus_event) kernel_event_signal(g_focus_event); } }

static uint32_t common_state(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0:                                                        // GetEventHandle → event signaled when a message is waiting
        if (!g_focus_event) { g_focus_event = kernel_event_new(0); applet_post(15); }   // 15 = FocusStateChanged: tells the title it is in focus
        out->copy[out->ncopy++] = g_focus_event; return 0;
    case 1:                                                        // ReceiveMessage → message id
        if (!g_msg_n) return R_NO_MESSAGE;
        OUT32(g_msg_queue[0]); memmove(g_msg_queue, g_msg_queue + 1, --g_msg_n * sizeof *g_msg_queue); return 0;
    case 5: OUT32(0); return 0;                                    // GetOperationMode: 0 = handheld
    case 6: OUT32(0); return 0;                                    // GetPerformanceMode: 0 = normal
    case 8: OUT32(0); return 0;                                    // GetBootMode: 0 = normal
    case 9: OUT32(1); return 0;                                    // GetCurrentFocusState: 1 = in focus
    case 60: OUT32(1280); OUT32(720); return 0;                    // GetDefaultDisplayResolution
    case 61: out->copy[out->ncopy++] = kernel_event_new(0); return 0;   // GetDefaultDisplayResolutionChangeEvent
    default: return unimpl("applet ICommonStateGetter", in);
    }
}
static uint32_t self_controller(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 1: case 2: return 0;                                      // LockExit / UnlockExit
    case 9: out->copy[out->ncopy++] = kernel_event_new(0); return 0;   // GetLibraryAppletLaunchableEvent
    case 10: case 11: case 12: case 13: case 14: case 16: case 17: case 18: case 19: return 0;   // screenshot permission, mode-change notifications, focus handling, restart message, ...
    case 40: OUT64(1); return 0;                                   // CreateManagedDisplayLayer → layer id
    case 50: return 0;                                             // SetHandlesRequestToDisplay
    default: return unimpl("applet ISelfController", in);
    }
}
static uint32_t window_controller(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 1: OUT64(0x1000); return 0;                               // GetAppletResourceUserId
    case 10: return 0;                                             // AcquireForegroundRights
    default: return unimpl("applet IWindowController", in);
    }
}
static uint32_t audio_controller(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session; (void)out;
    switch (in->cmd) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: return 0;   // SetExpectedMasterVolume ... SetTransparentVolumeRate: nothing listens
    default: return unimpl("applet IAudioController", in);
    }
}
static uint32_t display_controller(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session; (void)out;
    return unimpl("applet IDisplayController", in);
}
static uint32_t application_functions(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 21: OUT64(0x656e55534d4c);  return 0;                     // GetDesiredLanguage: a language code, "en-US" packed
    case 22: return 0;                                             // SetTerminateResult
    case 40: OUT32(1); return 0;                                   // NotifyRunning → true
    case 30: case 31: case 60: case 66: case 67: return 0;         // home-button blocking, media playback state, game-play recording
    default: return unimpl("applet IApplicationFunctions", in);
    }
}
static uint32_t library_creator(int session, const ipc_in_t *in, ipc_out_t *out) { (void)session; (void)out; return unimpl("applet ILibraryAppletCreator", in); }
static uint32_t winding_controller(int session, const ipc_in_t *in, ipc_out_t *out) { (void)session; (void)out; return unimpl("applet IProcessWindingController", in); }

static uint32_t debug_functions(int session, const ipc_in_t *in, ipc_out_t *out) { (void)session; (void)out; return unimpl("applet IDebugFunctions", in); }

static uint32_t application_proxy(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    struct { uint32_t cmd; const char *name; ipc_handler_t fn; } o[] = {
        {0, "ICommonStateGetter", common_state}, {1, "ISelfController", self_controller}, {2, "IWindowController", window_controller},
        {3, "IAudioController", audio_controller}, {4, "IDisplayController", display_controller}, {10, "IProcessWindingController", winding_controller},
        {11, "ILibraryAppletCreator", library_creator}, {20, "IApplicationFunctions", application_functions},
        {1000, "IDebugFunctions", debug_functions},
    };
    for (unsigned i = 0; i < sizeof o / sizeof *o; i++)
        if (o[i].cmd == in->cmd) { out->move[out->nmove++] = ipc_new_session(o[i].name, o[i].fn); return 0; }
    return unimpl("applet IApplicationProxy", in);
}
static uint32_t applet_oe(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: out->move[out->nmove++] = ipc_new_session("appProxy", application_proxy); return 0;   // OpenApplicationProxy(pid, process handle) → IApplicationProxy
    default: return unimpl("appletOE", in);
    }
}

// ---- fsp-srv: the file system. The title's data storage (RomFS) is read from an image on the host; the rest of the file systems are not served yet.
static int g_romfs_fd = -1;
void fsp_set_romfs(const char *path) {
    g_romfs_fd = open(path, O_RDONLY);
    if (g_romfs_fd < 0) fprintf(stderr, "[RED] fsp: cannot open the RomFS image %s\n", path);
}
typedef struct { const uint8_t *img; uint64_t size; } mem_storage_t;
// A RomFS with a root directory and nothing in it, for system data the host has no files of (firmware archives): mounting succeeds and every file is not found, as the title can handle.
static mem_storage_t *empty_romfs(void) {
    static mem_storage_t st; static uint8_t img[0x400];
    if (!st.img) {
        uint64_t h[10] = {0x50, 0x200, 12, 0x20c, 24, 0x224, 12, 0x230, 0, 0x400};      // header, then directory hash table / entries, file hash table / entries, file data
        memcpy(img, h, sizeof h);
        memset(img + 0x200, 0xff, 12);                                                   // directory hash table: three empty buckets
        uint32_t root[6] = {0, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0};       // the root entry: no parent, sibling, child, file or hash chain; empty name
        memcpy(img + 0x20c, root, sizeof root);
        memset(img + 0x224, 0xff, 12);                                                   // file hash table
        st.img = img; st.size = sizeof img;
    }
    return &st;
}
static uint32_t storage(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    const mem_storage_t *ms = in->data;
    if (ms) {                                                      // memory-backed storage (system data)
        switch (in->cmd) {
        case 0: {
            uint64_t off = in->args[0] | (uint64_t)in->args[1] << 32, len = in->args[2] | (uint64_t)in->args[3] << 32;
            if (!in->nrecv) return 0x2d6402u;
            uint8_t *dst = (uint8_t *)in->recv[0].addr; uint64_t n = len < in->recv[0].size ? len : in->recv[0].size;
            memset(dst, 0, n);
            if (off < ms->size) memcpy(dst, ms->img + off, n < ms->size - off ? n : ms->size - off);
            return 0;
        }
        case 2: return 0;
        case 4: OUT64(ms->size); return 0;
        default: return unimpl("fsp memory IStorage", in);
        }
    }
    switch (in->cmd) {
    case 0: {                                                      // Read(offset, length) → bytes into the output buffer
        uint64_t off = in->args[0] | (uint64_t)in->args[1] << 32, len = in->args[2] | (uint64_t)in->args[3] << 32;
        if (!in->nrecv || g_romfs_fd < 0) return 0x2d6402u;        // fs ResultInvalidArgument-like: no buffer / no image
        uint8_t *dst = (uint8_t *)in->recv[0].addr; uint64_t n = len < in->recv[0].size ? len : in->recv[0].size, done = 0;
        while (done < n) { ssize_t r = pread(g_romfs_fd, dst + done, n - done, off + done); if (r <= 0) break; done += (uint64_t)r; }
        if (done < n) memset(dst + done, 0, n - done);
        return 0;
    }
    case 2: return 0;                                              // Flush
    case 4: {                                                      // GetSize → u64
        off_t sz = g_romfs_fd >= 0 ? lseek(g_romfs_fd, 0, SEEK_END) : 0;
        OUT64((uint64_t)sz); return 0;
    }
    default: return unimpl("fsp IStorage", in);
    }
}
static uint32_t fsp(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 1: return 0;                                              // Initialize(pid)
    case 200: out->move[out->nmove++] = ipc_new_session("IStorage", storage); return 0;   // OpenDataStorageByCurrentProcess → IStorage of the RomFS
    case 22: case 25: { extern uint32_t fsfs_open_savedata(ipc_out_t *); ipc_out_t tmp = {0}; return fsfs_open_savedata(&tmp); }   // CreateSaveDataFileSystem*: the directory is made on first open
    case 51: case 52: case 53: return fsfs_open_savedata(out);      // OpenSaveDataFileSystem* → IFileSystem of the save data directory
    case 202: {                                                     // OpenDataStorageByDataId(storage, data id): system data (fonts, Mii resources) is firmware, which the host does not have
        uint64_t id = in->args[2] | (uint64_t)in->args[3] << 32;
        extern const uint8_t *mii_standin_romfs(size_t *);
        static mem_storage_t mii;
        if (id == 0x0100000000000802ull && !mii.img) mii.img = mii_standin_romfs((size_t *)&mii.size);       // MiiModel: stand-in files with every part absent
        out->move[out->nmove++] = ipc_new_session_data("IStorage", storage, id == 0x0100000000000802ull ? (void *)&mii : (void *)empty_romfs());
        return 0;
    }   // OpenDataStorageByDataId: system data (fonts, Mii resources) is firmware, which the host does not have: an empty RomFS
    case 203: return 0x7d402u;                                     // OpenPatchDataStorageByCurrentProcess: no update patch is installed (fs ResultTargetNotFound)
    case 1005: OUT32(0); return 0;                                 // GetGlobalAccessLogMode → none
    default: return unimpl("fsp-srv", in);
    }
}

// ---- nvdrv: the GPU driver's device nodes. NVN is answered above the driver (the host implements the NVN API itself), so the nodes only have to open and accept their ioctls.
static uint32_t g_nv_fd = 1;
static uint32_t nvdrv(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: {                                                      // Open(path) → fd, error
        const char *path = in->nsend ? (const char *)in->send[0].addr : "?";
        if (getenv("NX_IPC_TRACE")) fprintf(stderr, "[ipc] nvdrv Open %.40s → fd %u\n", path, g_nv_fd);
        OUT32(g_nv_fd++); OUT32(0); return 0;
    }
    case 1: case 11: case 12: OUT32(0); return 0;                  // Ioctl(fd, request, in, out) → error 0
    case 2: OUT32(0); return 0;                                    // Close(fd) → error
    case 3: OUT32(0); return 0;                                    // Initialize(transfer memory size, process and transfer memory handles) → error
    case 4: out->copy[out->ncopy++] = kernel_event_new(0); OUT32(0); return 0;   // QueryEvent(fd, event id) → event, error
    case 8: return 0;                                              // SetClientPID
    default: return unimpl("nvdrv", in);
    }
}

// ---- hid: input. The SDK's hid library reads a shared memory block of ring buffers; the host hands out a zeroed one, so every device reads as absent and every state list as empty.
// Commands that configure or activate something are accepted; one not listed is reported once and accepted too, since input is optional and a refusal makes a title stop.
static uint32_t hid_permissive(const char *what, const ipc_in_t *in) {
    static unsigned seen[64]; unsigned k = in->cmd & 63;
    if (seen[k] != in->cmd + 1u) { seen[k] = in->cmd + 1u; fprintf(stderr, "[RED] ipc: %s command %u is not implemented, answered with success and no data\n", what, in->cmd); }
    return 0;
}
static uint32_t applet_resource(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: hid_start(); out->copy[out->ncopy++] = ipc_new_session_data("shmem", NULL, hid_block()); return 0;     // GetSharedMemoryHandle → the 0x40000-byte input block, refilled by hid_input.c
    default: return unimpl("hid IAppletResource", in);
    }
}
static uint32_t g_npad_styles;
static uint32_t vibration_list(int session, const ipc_in_t *in, ipc_out_t *out) {   // IActiveVibrationDeviceList: ActivateVibrationDevice(handle) is the only command
    (void)session; (void)out;
    if (in->cmd == 0) return 0;
    return unimpl("hid IActiveVibrationDeviceList", in);
}
static uint32_t hid(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: out->move[out->nmove++] = ipc_new_session("hid.res", applet_resource); return 0;   // CreateAppletResource(aruid) → IAppletResource
    case 100: g_npad_styles = (uint32_t)in->args[0]; return 0;       // SetSupportedNpadStyleSet(style set, aruid)
    case 101: OUT32(g_npad_styles); return 0;                      // GetSupportedNpadStyleSet → what the title set
    case 102: case 103: case 104: return 0;                        // SetSupportedNpadIdType / ActivateNpad / DeactivateNpad
    case 106: out->copy[out->ncopy++] = kernel_event_new(0); return 0;   // AcquireNpadStyleSetUpdateEventHandle → event
    case 108: OUT64(0); return 0;                                  // GetPlayerLedPattern
    case 121: OUT64(0); return 0;                                  // GetNpadJoyHoldType
    case 11: case 66: case 67: case 120: case 122: case 123: case 124: case 125: case 126: case 127: case 128: return 0;   // touch screen, six-axis sensor, joy hold type, joy assignment, handheld mode: nothing to configure
    case 203: out->move[out->nmove++] = ipc_new_session("hid.vib", vibration_list); return 0;   // CreateActiveVibrationDeviceList
    default: return hid_permissive("hid", in);
    }
}

// ---- set: language and region. English (US), the console's default region USA.
static const char *const g_langs[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant"};
static uint64_t lang_code(const char *s) { uint64_t v = 0; memcpy(&v, s, strlen(s) < 8 ? strlen(s) : 8); return v; }   // LanguageCode: the ASCII string in a u64, zero padded
static uint32_t set_service(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    unsigned n = (unsigned)(sizeof g_langs / sizeof *g_langs);
    switch (in->cmd) {
    case 0: OUT64(lang_code("en-US")); return 0;                    // GetLanguageCode
    case 1: case 5: {                                              // GetAvailableLanguageCodes → count; the codes into the output buffer
        uint32_t cap = in->nrecv ? in->recv[0].size / 8 : 0, k = cap < n ? cap : n;
        for (uint32_t i = 0; i < k; i++) { uint64_t c = lang_code(g_langs[i]); memcpy((uint8_t *)(uintptr_t)in->recv[0].addr + 8 * i, &c, 8); }
        OUT32(k); return 0;
    }
    case 2: OUT64(lang_code(in->args[0] < n ? g_langs[in->args[0]] : "en-US")); return 0;   // MakeLanguageCode(language)
    case 3: case 6: OUT32(n); return 0;                             // GetAvailableLanguageCodeCount
    case 4: OUT32(1); return 0;                                    // GetRegionCode: 1 = USA
    case 8: OUT32(0); return 0;                                    // GetQuestFlag
    default: return unimpl("set", in);
    }
}

// ---- mii: the Mii database. Empty: no Miis are stored, but one can be built: a default character (the values the Mii editor starts from), since a title that asks for one and gets
// nothing leaves a half-built model. A CharInfo is 0x58 bytes.
static void mii_default_charinfo(uint32_t out[22]) {
    uint8_t c[0x58] = {0};
    for (int i = 0; i < 16; i++) c[i] = (uint8_t)(0x11 * (i + 1) ^ 0x5a);                    // create id: any non-zero UUID
    c[0x10] = 'M'; c[0x12] = 'i'; c[0x14] = 'i';                                              // nickname "Mii" (UTF-16LE)
    enum { FONT = 0x26 };
    static const uint8_t v[] = {                                                              // from font_region: font, colour, gender, height, build, type, region move,
        0, 0, 0, 64, 64, 0, 0,
        0, 0, 0, 0,                                                                           // faceline type, colour, wrinkle, make
        33, 0, 0,                                                                             // hair type, colour, flip
        0, 8, 4, 3, 4, 2, 12,                                                                 // eye type, colour, scale, aspect, rotate, x, y
        0, 0, 4, 3, 6, 2, 10,                                                                 // eyebrow type, colour, scale, aspect, rotate, x, y
        0, 4, 9,                                                                              // nose type, scale, y
        0, 19, 4, 3, 13,                                                                      // mouth type, colour, scale, aspect, y
        0, 0, 0, 4, 10,                                                                       // beard colour, beard type, moustache type, scale, y
        0, 0, 4, 10,                                                                          // glasses type, colour, scale, y
        0, 4, 2, 20,                                                                          // mole type, scale, x, y
    };
    memcpy(c + FONT, v, sizeof v);
    memcpy(out, c, sizeof c);
}
static uint32_t mii_database(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: OUT32(0); return 0;                                    // IsUpdated(flag) → false
    case 1: OUT32(0); return 0;                                    // IsFullDatabase → false
    case 2: OUT32(0); return 0;                                    // GetCount(source) → 0
    case 3: OUT32(0); return 0;                                    // Get(source) → 0 entries
    case 4: return 0x1d0a02u;                                      // Get1 / lookups: no such Mii
    case 5: {                                                      // UpdateLatest(CharInfo, source) → the same CharInfo: nothing newer exists. (An error here made Mario Kart 8 Deluxe free the model resources it went on using.)
        uint32_t ci[22]; if (in->nargs >= 22) memcpy(ci, in->args, sizeof ci); else mii_default_charinfo(ci);
        for (int i = 0; i < 22; i++) OUT32(ci[i]); return 0;
    }
    case 6: case 7: {                                              // BuildRandom / BuildDefault → the default character, or (NX_MII_BUILD=0) an error so the title builds no Mii models
        static int mode = -1; if (mode < 0) { const char *e = getenv("NX_MII_BUILD"); mode = e ? atoi(e) : 1; }
        if (!mode) return 0x1d0a02u;
        uint32_t ci[22]; mii_default_charinfo(ci); for (int i = 0; i < 22; i++) OUT32(ci[i]); return 0;
    }   // BuildRandom / BuildDefault → the default character
    default: return hid_permissive("mii database", in);
    }
}
static uint32_t mii_static(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    switch (in->cmd) {
    case 0: out->move[out->nmove++] = ipc_new_session("mii.db", mii_database); return 0;   // GetDatabaseService(key code) → IDatabaseService
    default: return hid_permissive("mii", in);
    }
}

// ---- prepo: play reports (telemetry the console sends to Nintendo). Accepted and dropped.
static uint32_t prepo(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session; (void)out;
    if (in->cmd == 10100 || in->cmd == 10101 || in->cmd == 10102 || in->cmd == 10103 || in->cmd == 10104 || in->cmd == 10105 || in->cmd == 10200 || in->cmd == 20100 || in->cmd == 30100 || in->cmd == 40100 || in->cmd == 50100 || in->cmd == 90100) return 0;
    return hid_permissive("prepo", in);
}

// ---- pl:u: the shared fonts. They are firmware data the host does not have, so every font type is the host's own TrueType font (NX_SHARED_FONT, or fontconfig's sans) in the layout the
// SDK reads: per font, 0x18029a7f, the size xor 0x49621806 (big endian), then the font. A title draws its text with a different face than on the console; its metrics are close enough.
enum { FONT_TYPES = 6, SHARED_FONT_BLOCK = 0x1100000 };
static struct { uint8_t *mem; uint32_t off[FONT_TYPES], size; } g_font;
static ipc_shmem_t g_font_blob;
static int font_load(void) {
    if (g_font.mem) return g_font.size != 0;
    g_font.mem = calloc(1, SHARED_FONT_BLOCK);
    char path[512] = "";
    const char *e = getenv("NX_SHARED_FONT");
    if (e) snprintf(path, sizeof path, "%s", e);
    else { FILE *f = popen("fc-match -f '%{file}' sans 2>/dev/null", "r"); if (f) { if (!fgets(path, sizeof path, f)) path[0] = 0; pclose(f); } }
    FILE *f = path[0] ? fopen(path, "rb") : NULL;
    if (!f) { fprintf(stderr, "[RED] pl:u: no TrueType font for the shared fonts (set NX_SHARED_FONT)\n"); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *ttf = malloc((size_t)n);
    if (fread(ttf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(ttf); return 0; }
    fclose(f);
    uint32_t off = 0;
    for (int i = 0; i < FONT_TYPES && off + 8 + (uint32_t)n <= SHARED_FONT_BLOCK; i++) {
        uint32_t magic = 0x18029a7f, enc = (uint32_t)n ^ 0x49621806u; enc = __builtin_bswap32(enc);
        memcpy(g_font.mem + off, &magic, 4); memcpy(g_font.mem + off + 4, &enc, 4); memcpy(g_font.mem + off + 8, ttf, (size_t)n);
        g_font.off[i] = off; off = (off + 8 + (uint32_t)n + 0xf) & ~0xfu;
    }
    free(ttf);
    g_font.size = (uint32_t)n; g_font_blob = (ipc_shmem_t){g_font.mem, SHARED_FONT_BLOCK};
    fprintf(stderr, "pl:u: shared fonts are %s (%u bytes)\n", path, g_font.size);
    return 1;
}
static uint32_t pl(int session, const ipc_in_t *in, ipc_out_t *out) {
    (void)session;
    uint32_t type = in->args[0] < FONT_TYPES ? in->args[0] : 0;
    switch (in->cmd) {
    case 0: font_load(); return 0;                                 // RequestLoad(type)
    case 1: OUT32(font_load() ? 1 : 0); return 0;                  // GetLoadState(type) → 1 loaded
    case 2: font_load(); OUT32(g_font.size); return 0;             // GetSize(type): the font's bytes
    case 3: font_load(); OUT32(g_font.off[type] + 8); return 0;    // GetSharedMemoryAddressOffset(type): where the font starts, after its 8-byte header
    case 4: font_load(); out->copy[out->ncopy++] = ipc_new_session_data("shmem", NULL, &g_font_blob); return 0;   // GetSharedMemoryNativeHandle
    case 5: {                                                       // GetSharedFontInOrderOfPriority(language) → (loaded, count), buffers: types, offsets, sizes (u32 each), one per font
        if (!font_load()) { OUT32(0); OUT32(0); return 0; }
        uint32_t n = FONT_TYPES;
        for (unsigned b = 0; b < 3 && b < in->nrecv; b++) {
            uint32_t *w = (uint32_t *)(uintptr_t)in->recv[b].addr; unsigned cap = in->recv[b].size / 4;
            for (unsigned i = 0; i < n && i < cap; i++) w[i] = b == 0 ? i : b == 1 ? g_font.off[i] + 8 : g_font.size;
        }
        OUT32(1); OUT32(n); return 0;
    }
    default: return unimpl("pl:u", in);
    }
}

__attribute__((constructor)) static void register_services(void) {
    (void)ok_all;
    ipc_register_service("lm", lm);
    ipc_register_service("apm", apm);
    ipc_register_service("aoc:u", aoc);
    ipc_register_service("pctl:a", pctl); ipc_register_service("pctl:s", pctl); ipc_register_service("pctl:r", pctl); ipc_register_service("pctl", pctl);
    ipc_register_service("appletOE", applet_oe);
    ipc_register_service("fsp-srv", fsp);
    ipc_register_service("hid", hid);
    ipc_register_service("set", set_service);
    ipc_register_service("pl:u", pl);
    ipc_register_service("prepo:a", prepo); ipc_register_service("prepo:u", prepo); ipc_register_service("prepo:m", prepo); ipc_register_service("prepo:s", prepo);
    ipc_register_service("mii:u", mii_static); ipc_register_service("mii:e", mii_static);
    ipc_register_service("nvdrv", nvdrv); ipc_register_service("nvdrv:a", nvdrv); ipc_register_service("nvdrv:s", nvdrv);
}
