# Horizon services (nxnative's hle_*.c), port status

A game links the SDK into its own NSOs, so the service layer is not IPC emulation: the host takes over the SDK's `nn::` client functions by their mangled names (`HLE` for an
import nobody defines, `HLE_FORCE` for one a loaded module defines too; the loader binds every reference to a forced name to the host function, `hle_is_forced` in module.c).
The ported files live in `src/host/svc/` and build into `nxsvc`; `tests/guest/services.c` calls them from guest code (`ctest services`).

## Done (compile, link and are exercised by the guest test where noted)
| File | What it covers | Tested |
|---|---|---|
| hle_time.c | `nn::time` clocks, calendar conversion | yes (user clock, calendar, steady clock, the forced override) |
| hle_sf.c | `nn::sf::hipc` sessions for the services the host serves | yes (connect, refuse, close) |
| hle_sdkinit.c | startup calls of the SDK that need no answer | compiled |
| hle_account.c, hle_friends.c, hle_net.c, hle_nvdrv.c, hle_oe.c | accounts, friends, sockets stubs, nvdrv, the `oe` environment | compiled, not called yet |

## Shims that stand in for something missing
- `svc/events_stub.c`: service SystemEvents (`hle_bind_system_event` / `signal` / `clear`) are bound by calling the SDK's own `nn::os::AttachSystemEvent` on the guest thread
  (`cpu_call32` / `cpu_call64`) with a kernel event the host owns, then signalled from any thread.
- `hostsvc.c`: `dm_guest_sleep_ns`, `guest_clock_freeze_for`, `dm_thread_native_wait`, `dm_audio_suspend` are plain or empty.
- `compat.h`: the Apple calls the code uses (`clock_gettime_nsec_np`, `mach_absolute_time`, `mach_timebase_info`, `sys_icache_invalidate`).

## To port, in the order a boot meets them
hle_misc.c, hle_os.c (clocks and side table are in hostsvc.c), hle_fs.c (the big one: host file system + RomFS), hle_ro.c (nn::ro, needs the loader's NRO path), hle_hid.c (input via SDL),
hle_audren.c and hle_audio.c (audio via SDL), hle_swkbd.c, hle_touch.c, hle_mii.c, hle_codec.c, hle_oodle.c, hle_movie*.c (ffmpeg), hle_offline.c, hle_sdk16.c, hle_uemalloc.c,
hle_thread.c and hle_svc.c (replaced by src/host/kernel.c; only the pieces the services call remain to be moved), hle_fpslock.c, hle_haptics.c, hle_lifecycle.c.
A C `...` variadic hook cannot be used here (`hle_call` carries registers only). `HLE_FORCE` entries are 32 bytes on purpose (see hle.h).

## Retail titles: services answered at the IPC level (`ipc.c`, `svc/ipc_*.c`)
A retail title (started at its `rtld`, not through the symbol hooks above) talks to the services through its own SDK, so the host answers the HIPC/CMIF requests of `SendSyncRequest`:
sm:, lm, apm, aoc:u (`ListAddOnContent` family; the list-changed event is command 7 in the 1.0.0 SDK, 8 in the 4.x one), pctl, appletOE, fsp-srv (data storage from the RomFS image,
save data under `work/save/<title>`, system data: the MiiModel stand-in, anything else "not found"), nvdrv (a stub; command 13 is not implemented), hid (a zeroed shared memory; commands 11, 100,
102, 103, 120 and 128 are answered empty), set, mii (`mii:u`/`mii:e` database: empty, `BuildRandom`/`BuildDefault` return a default CharInfo), prepo, pl:u (commands 0 to 5), audren:u (the 1.0.0 and SDK 4
formats). Symbol hooks that still apply to a retail title: nn::vi / nvn loader / nn::oe notification and theme calls (`hle_vi32.c`, `hle_oe32.c`, `hle_oe.c`), nn::account (`hle_account32.c`),
and the two Mii validity checks (`hle_mii32.c`). `docs/mk8d-bringup.md` lists what a title needed and how each part was found.
