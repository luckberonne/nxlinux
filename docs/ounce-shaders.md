# Ounce shaders (Switch 2 titles)

State on 2026-11-13. The Zelda Ocarina of Time `.nup` (see `docs/running.md`) is a Switch 2 build: its RomFS has only `*.Nin_Ounce_NVN2.bfsha` shader archives, SM86 (Ampere) machine
code, which the Maxwell translator (`tools/dmsh_xlate`) cannot read. This is what is known and what exists.

## Format

- `Shader/*.bfsha.zs`: zstd with the dictionary `zs.zsdic` out of `Pack/ZsDic.pack.zs` (a SARC), then an FSHA archive holding a BNSH.
- A stage binary is an NVN control section (magic `0x98761234`, `0xe0` bytes: the shader's own 16-byte hash at +0x20, the code area size at +0x38) and a code area: a `0x80`-byte
  program header (SPH version 4: its first 0x50 bytes keep Maxwell's layout, so `ShaderHeader` parses stage, kill flag and the pixel output map) followed by SM86 instructions of
  16 bytes. A record `{u64 control offset, u64 code offset}` (offsets from the BNSH) ties them; about 14 % of the control sections have no record and are skipped.
- Unlike Maxwell, the same control magic is the real thing (`hle_nvn.c` counts it as "the title's dummy control section"), and `keys_hash` (control +0x704, 0x880 bytes) does not apply:
  the key of an Ounce stage is the hash stored in its control section.
- Conventions seen in the code: attribute addresses as on Maxwell (position at 0x70, generics from 0x80); `c[3]` is the first user uniform block; textures through `TEX.SCR`
  with immediate texture and sampler indexes (and bindless `.B` forms); global memory through `LDG` with 64-bit addresses held in constant buffers (`LDC.64`), not through
  `c[0]` storage buffer tables; `BSSY`/`BSYNC` convergence barriers, `CALL`/`BRX`, uniform registers `UR` and `ULDC`.
- The whole library of Ocarina of Time: 15,255 distinct stage binaries (1,422 vertex, 13,898 pixel), about 60 mnemonics, no encoding nvdisasm rejects.

## Tools

| | |
|---|---|
| `tools/ounce_extract.py ROMFS.img OUT` | decompresses the archives and writes `<hash>.code` and `<hash>.ctl` per program |
| `tools/ounce_sass.py OUT` | `<hash>.sass`: the text `nvdisasm` prints (needs the CUDA toolkit; a decoder of its own would drop that) |
| `tools/ounce_xlate` (C#) | `ounce_xlate CODE SASS OUT.spv`, or `--dir OUT` to translate every program and count the missing mnemonics |
| `third_party/ryujinx/.../Translation/Sm86/` | the frontend: `SassText` (parser), `Sm86Emitter` and `Sm86Attributes` (semantics), `Sm86Translator` (entry) |

The frontend builds the same IR the Maxwell instructions do, so SSA, the optimizer and the SPIR-V generator are the Ryujinx ones. Changes to the vendored sources: `Translator.EmitCustom`
and `TranslatorContext.TranslateCustom` (the entry), `RegisterConsts.SsaGprs/SsaPreds` and `Ssa.cs` (room for the uniform registers: UR n is GPR index 256 + n, UP n predicate 8 + n),
and four helpers made `internal` (`FixedFuncToUserAttribute`, `EmitLoadConstant`, `TryFixedFuncToUserAttributeIpa`, `TryConvertIdToIndexForVulkan`).

## Done

MOV, IMAD (plain and `.WIDE`), IADD3 (also carry in with `.X`, and a carry out when the third input is zero), LEA (plain, with carry out, `.HI[.X]`), LOP3 (value and predicate forms), SHF,
SGXT, IMNMX, SEL/FSEL, ISETP/FSETP/FSET, PLOP3, PRMT (constant selector), FADD/FMUL/FFMA, FMNMX, MUFU (RCP RSQ SQRT EX2 LG2 SIN COS), FRND, F2I, I2F/I2FP, HADD2.F32, FLO, POPC, CS2R
(zero), LDC (constant and register-indexed), AST, ALD, IPA (pass/centroid/constant), SHFL, VOTEU, LDL/STL (one word), P2R, S2R (lane id), KILL, BSSY/BSYNC/BREAK, BRA (also
`BRA Pc, target`), EXIT, NOP/DEPBAR/WARPSYNC, and the uniform forms of the integer ones (UMOV, UIADD3, USHF, UISETP, USEL, ULEA, R2UR, ULDC). Code after an unconditional KILL, EXIT or
branch is dropped until the next label. The prescan sets the same feature flags the Maxwell decoder does (a missing `Shuffle` flag crashed the generator).

Measured over the whole library (15,255 programs, mnemonic census): 734 programs use only what is done above. What the others need, by programs: TEX 14,183, LDG 13,500, CALL 11,671, BRX
11,499, FSWZADD 3,393, TLD4 1,416, then a few dozen each of TXQ, TXD, R2P, UFLO, RED, TMML, STG. Only TEX alone blocks 967 programs, LDG alone 334.

With `OUNCE_SKIP=1` (unsupported instructions skipped) all 402 sampled programs translate and all 402 SPIR-V modules pass `spirv-val`, so what exists is type-correct IR; with the unsupported
ones counted, 26 of the 402 translate completely. The two SceneMaterial programs give the expected code.

Textures (`TEX`, `TLD4`; `Sm86Texture.cs`): the operand layout was read off nvdisasm's output for CUDA texture fetches (`tex.2d`, `tex.3d`, `tex.cube`, `tex.a2d`, `tld4`, `tex.level`), whose SASS is the same
dialect. The sources are one sequence: array layer, coordinates, then lod/bias, offsets, depth reference; `Ra` holds the first two elements, `Rb` the rest and the extras. The results are the mask's
components in ascending order into `Rlo, Rlo+1` (the second register printed) and then `Rhi, Rhi+1` (the first printed). The bound form's immediates are the constant buffer slot and the word offset of the
handle: `c[2]`, word 8 + 2n is the NVN driver's combined-handle table, so the binding is the one Maxwell's bindless read of that word gives (`fp_t_cb2_8` and the like in the SPIR-V). Also done:
the bindless `.B` form (handle in `Rb`'s first register), a handle in a uniform register (`TEX ..., UR10, 0x0, 2D`), `.LL .LZ .LB .DC`, array/3D/cube, gather with `.R/.G/.B/.A`. Not done: `TXD`, `TMML`, `TXQ`,
offsets (`.AOFFI`), multisample. The order of the extras (lod, offsets, depth reference) when several are present is a guess from the Maxwell encoding.

Global memory (`LDG`, `STG`; `Sm86Memory.cs`): the library's addresses are `[R.U32+URn(+imm)]` (a word offset plus the 64-bit pointer the driver puts in `c[0][0x510]`, `0x520`, `0x530`, `0x540`; `0x110` and
`0x120` in a few), `[URn+imm]`, `[R]`/`[R+imm]` (a 64-bit pair, pointers read from memory). They become a new IR storage kind `StorageKind.PhysicalMemory` (operands: address low, address high, value): the SPIR-V backend
builds an `OpConvertUToPtr` to `PhysicalStorageBuffer` and `OpLoad`/`OpStore ... Aligned 4` (`Module.LoadAligned/StoreAligned` in `Spv.Generator/ModuleAligned.cs`), and a program that has one switches to
addressing model `PhysicalStorageBuffer64` with capabilities `PhysicalStorageBufferAddresses` and `Int64` (`StructuredProgramInfo.UsesPhysicalMemory`). Widths 64 and 128 are 2 and 4 word accesses; `.U8/.S8/.U16/.S16`
load the aligned word and extract. Ryujinx's global-to-storage pass does not touch this kind. nxlinux's NVN pools already report Vulkan buffer device addresses as GPU addresses (`nvn_vk.c`), so the pointers the host
writes into `c[0]` are directly valid. `RED` is not done.

The bindless handles in the library are read through those tables (`ULDC.64 UR4, c[0x0][0x540]`, `LDG.E.64.CONSTANT R2, [R2.U32+UR4]`). `BindlessElimination` accepts a handle that is a `PhysicalMemory` load (one
added condition) and, as for Maxwell, turns it into `texture index = handle & 0xfffff`, `sampler index = handle >> 20` over arrays; the one thing it needs from the accessor is
`QueryHostSupportsSeparateSampler() = true` (without it every bindless read was dropped, which is what the 240 "Failed to find handle source" programs were). The host side is the open part: the descriptor
arrays (every NVN texture pool entry, indexed by the 20-bit texture id) and the `c[0][0x510..0x540]` pointers.

Control flow and the rest of the common instructions (2026-11-14): `CALL.REL.NOINC `(L)` pushes nothing and 312 of the 314 uses target a `BSYNC`, so it is a jump (`Branch`). `BRX Rn imm` is a `switch`: the shader
loads a table entry from `c[1]` (`LDC Rn, c[0x1][Ri+off]`), the target is entry + imm + the next instruction's address, and the entries are instruction addresses, so the immediate is minus that address. The
tables are in the program: the **control section from +0xe0** is a count and then, per table, `{address of the BRX, entries, flags 0x10000, offset in c[1]}`, and **c[1] is the end of the code area**: control +0x40 is
where it starts and +0x44 where the area ends (the code size at +0x38 stops before it; `ounce_extract.py` now writes `<key>.c1`). The targets have no label in nvdisasm's listing, so the translator makes one at
each, and the jump is a chain of comparisons with the distinct entries (the last unconditional). `Sm86Translator.Translate` takes the control section and c[1] as optional arguments; the host's c[1] binding
(library kind 7) supplies the same bytes at run time. `FSWZADD Rd, Ra, Rb, PNNPPNNP`: two letters per lane in lane order, the first for Ra and the second for Rb (P add, N subtract, Z drop); the shaders pair it
with `SHFL.BFLY` (Ra = the neighbour's value, Rb = its own), and only this reading gives right minus left for `PNNP...` and lower minus upper for `PNPNNPNP`. `TXD Rhi, Rlo, Ra, Rb, <cbuf>, <word>, <dim>[, <mask>]`
(read off CUDA `tex.grad.2d` / `tex.grad.a2d`; the other shapes lower to TEX): Ra the coordinates, Rb the derivatives interleaved by coordinate (dsdx, dsdy, dtdx, dtdy), the array layer in Rlo, results as TEX.
Measured on a random 4,386 of the 15,255 programs: 4,363 translate completely (99.5 %) and every one passes `spirv-val`. Missing in the rest: TXQ, R2P, RED (add, min, max), the `ISBERD`/`ISBEWR`
attribute instructions, `S2R SR_LTMASK`, TMML, `ULDC.U8`, `RET.REL.NODEC`, uniform predicates (`UP2UR`, a UR as a guard), `VOTE.ALL`, `UBREV`, `REDUX`, `LDL.128`/`STL.128`, `TLD4.B`.
`AST a[0x5c]` (41 programs; a vertex output) is dropped with a note.

Library and pointers (2026-11-14): `ounce_xlate --dir` now writes `<key>.res.json` beside each `.spv` (the format of tools/dmsh_xlate; stage from the SPH, bits 10..13 of word 0: 1 vertex, 5 pixel), and
`tools/shader_pack.py` packs them as is: the `fp_c0` block of a shader that reads `c[0][0x510..]` becomes a driver-segment binding (kind 4), which the host already fills per draw with the stage's storage buffer table
(`dmsh_abi.h`: address + size at `0x110 + 0x10 r` for the vertex stage, `0x510 + 0x10 r` for the pixel stage), so the pointers need no new host code. The bindless handle arrays come out as the 32-entry texture and
sampler arrays the Unity/Unreal path already feeds (`TEXA`/`SAMPA`, element k = the pool entry k). The host finds a shader by `keys_hash` (hle_nvn.c), which for a control section with magic 0x98761234 is the
first 8 bytes of the hash at `+0x20`, the name the extractor gives. Try it: `--shaders ounce.nxsl` on the run command (101 of the 402 sampled programs packed; the title boots, builds 5,501 programs and stops at
`nifm:u` before any draw, so nothing has been looked up yet). Update, same day: the title no longer stops there. Four things stood in the way and are done: the system 20 SDK's npad reader is a seqlock (it retries while an entry's counter `rec+0` is odd: `hid_input.c` writes `2n`, with `rec+8` the consecutive sampling number); `nsd:a` and `nifm:u` are served offline (`ipc_services.c`); `nn::socket::EventFd` is a real host eventfd and `Poll(pollfd*)` waits on it (`hle_net.c`; module.c lets every `nn::socket` 4-byte wrapper be hooked); `nn::oe::GetProgramTotalActiveTime` is hooked. With `--shaders` the title now issues draws and presents frames headless (`frame 1, 2, 3`). Which draws find a shader in the 101-program library has not been looked at.

Whole library and first draws (2026-11-14): `ounce_extract.py` -> `ounce_sass.py` -> `ounce_xlate --dir` -> `shader_pack.py` over all 15,255 programs (about 6 minutes of nvdisasm, 5 of translation, 1 of packing; keep the
working directory off `/tmp`, which is a 7.5 GB tmpfs: the SASS alone is 4.5 GB, so `work/run/ounce_all` is used). 15,170 programs translate (99.4 %), every module passes `spirv-val`, and the library is 2.5 GB. Bindings must
follow the host's single descriptor set: the vertex stage's start at 1 and the fragment stage's at 64 (`ounce_xlate`'s accessor reads the stage from the SPH, as `dmsh_xlate` takes it from the job). The raw pointers in
`c[0]` have to be Vulkan device addresses: `driver_buffer` (`nvn_draw_vk.c`) converts each storage-buffer slot's guest address with `nvn_vk_device_address` (`nvn_vk.c`: pool `gpu + (addr - abase)`) when the title is a
Switch 2 one (`dm_switch2_gfx`); with the guest addresses the GPU faulted (`GPUVM fault`, `vkQueueSubmit2 failed (-4)`). With that the title builds pipelines from the translated shaders and makes draws without GPU or validation errors. What kept the frames black, in the order found: (1) the title draws almost everything with
**8-bit indices**, which the backend skipped (`nvn_draw_idx` now widens them into the transient arena); (2) programs made with the **control section not yet filled** (magic, no hash) were keyed by zeros: such a
stage is now a placeholder (`code_reserved`) until the hash is there; (3) **programs the title builds itself** have a control section with magic and code size (+0x38) but a **zero hash**: they are keyed by
the first 8 bytes of the SHA-256 of their code (`keys_hash`) and `NX_SHADER_DUMP=<dir>` writes them (`.code`, `.ctl` with its own size from +0x30, `.c1`) for the same offline chain; the title has about 2,500 of
them (1,796 translate: the rest are compute shaders, whose SPH gives stage 14, and instructions not done yet: R2P, IMAD.HI.U32, TLD, F2FP.PACK_AB, SUST, LDS/STS, HADD2/HMUL2, HMMA ...). Without them no
draw reached the window textures (they are the compositing step). With the archive library (15,204 programs) and these, the window textures get draws and the frame shows two white bars
(`NX_FRAME_DUMP`): a loading screen whose textures are 4x4 placeholders. The title then stays there (frame counter past 30,000, the intermediate RGB10A2 target unchanged): it is waiting for something
the host does not give yet (the video thread's null call in `movie::Dictionary::SetInt32`, `time:u`, asynchronous texture loading are the suspects).

## To do, in the order that matters

1. Textures: `TXD`, `TMML`, `TXQ`; the host side of the c[2] bound handles, of the bindless descriptor arrays and of the c[0][0x510..0x540] pointers.
2. `RED` (atomics through device addresses).
3. The instructions of the list above (TXQ, R2P, RED, ISBERD/ISBEWR, SR_LTMASK, TMML ...), then a translation of the whole library with the translator's SPIR-V checked on the GPU.
4. A library writer for the host (`shader_pack.py` takes `.spv` + `.res.json`): resource bindings, c[3+] uniform blocks, the key from the control hash, and the runtime side
   (`keys_hash` for Ounce control sections, the program builder is done).
5. Check numerics on the GPU: only `spirv-val` has looked at the output so far.

## The title screen is a movie (2026-11-15)

After the inventory assets, Ocarina of Time streams `Event/Movie/Dm_Title_0005.webm` (AV1, 1920x1080) and waits for it, so the title idles until a video decoder works. The movie library
(`movie::VideoDecoder` in subsdk0) drives `nn::codec`'s hardware decoder, which needs the graphics supervisor's user session (`nn::gsv::GetUserSession`, null on the host).
`src/host/svc/hle_codec_video.c` replaces the decoder API with ffmpeg and `src/host/svc/hle_gs.c` replaces `nn::syncpt`. What the library expects:

- Output buffers sit in a queue: `RegisterDecoderBuffer` and `ReleaseDecoderOutputBuffer` add one; `AcquireDecoderOutputBuffer` (the library calls it right after either, and it never waits) takes the next
  one and returns a fence, a 16-byte sync point handle that is reached when a picture has been written into that buffer.
- The library keeps the acquired buffers in order, polls the first one's fence with `TryWaitSyncPoint` and asks a sync point event for it (`RequestSyncPointEvent`; `CancelSyncPointEvent` clears the event).
  Its event manager thread (`VideoDecoderEventManager::Loop`) wakes on that event and runs a callback that reads the picture's properties and hands the frame to the title.
- `GetDecoderSubmissionReadyEvent` is a SystemEvent the title waits on before each `SubmitDecoderInputBuffer`; `ReadDecoderOutputImage{Nv12,Rgba}(pool, index, x, y, w, h, dst, stride, size, ...)` copies a picture out.
- Hooks of functions that return a `Result` must return one: a `void` hook left garbage in x0, `CreateSyncPointEvent`'s caller checks it and the title aborted at random (the old one-run-in-four abort).
- A function under 16 bytes is only taken over if it is in the exception list of `module_hook_exports` (module.c): `nn::time::Initialize`, `nn::socket`, `nn::codec`.

`NX_CODEC_LOG=1` traces the decoder and the sync points. Status: pictures decode and are read (more than a thousand in 80 s), the title no longer stalls, and the window still shows only the two white bars: the pass
that draws the video is the next thing to look at.

## Compute programs (2026-11-15)

A compute program has SPH shader type 14 (word 0 bits 10..13). `Sm86Translator` builds `ShaderStage.Compute` definitions for it (and adds `TranslationFlags.Compute`); the work group size and the shared
memory size come from the control section: **+0x58, +0x5c, +0x60 = local size x, y, z; +0x64 = shared memory bytes** (`ounce_xlate` reads them, `Accessor.QueryComputeLocalSize*`). Added for them: `LDS`/`STS`
(byte, half, word, 64 and 128 bit, `[R.X4+off]` addressing, shared memory through `InstEmit.EmitLoad/EmitStore`), `S2R SR_TID.X/Y/Z`, `SR_CTAID.X/Y/Z` and `S2UR`, `TLD` (texel fetch: `TEX` with integer
coordinates). A `@UPT` guard is accepted. Of the 479 compute programs the title built itself, 416 translate (`work/run/cs_own`); the rest need `SUST`, `F2FP.PACK_AB`, packed half-precision math (`HADD2`...),
`IMAD.IADD/X/HI.U32`, `TLD4.F16`, and tensor-core instructions (`HMMA`, `LDSM`, `MOVM`).

The compute programs a dispatch looks for and does not find (`NX_SKIP_LOG=1` prints their key, control hash and sizes) are **not in the Shader/*.bfsha archives**: they are in `Movie/Ndve/Ndve-Nvn2-vid_seq5.dat.zs`,
the movie engine's video enhancement network (NDVE: `HMMA.16816.F16`, `LDSM`, `STS.128`, 7 of its 11 programs use tensor cores). Other containers hold controls too: `Effect/*.vsetb.byml.zs` (33,497 control
sections, no hash: keyed by the code's SHA-256) and `*.release.sarc.zs` (227 programs with a hash). In these the **code area follows its control section at once** (control +0x30 is the section's size, +0x38 the
code size): `tools/ounce_extract.py --others [--only NAME]` extracts them (`adjacent_programs`). `NX_SHADER_DUMP_HASHED=1` makes the host dump hashed programs too, to find where one lives.


## What the title does after the movie (2026-11-15, later)
The main thread (0x51, `WaitLightEvent` on a scheduler object) is not stuck: `NX_TRACE_ADDR=<hex>` (kernel.c; WaitForAddress/SignalToAddress on one address, first 400)
shows job-system workers (0x8d-0x97) signalling it about 20 000 times a second. The game runs about 60 frames/s and keeps going for minutes.
- Per frame it makes about 14 draws (30 indices each) into an offscreen 2560x1440 target and one 6-index blit into the window, so the window only ever holds two white bars.
  Those draws sample the 4x4 texture `id258` in every slot; the logo texture (`tex5119`, 1664x1280) and others are loaded but not drawn. Text render targets (285x94) come out all white.
- The movie is painted by compute only: 11 dispatches per decoded picture (the 11 NDVE programs, none translated, so every one is skipped: ~250 skips/s). The inputs of the first one
  (`1fadca19`, 960x540, local 16x2) are the NV12 picture and a 64-byte push block holding a YUV->RGB matrix; the push block of the other (84 bytes) holds 1919x1079 and 2559x1439.
- `nvnCommandBufferPushComputeConstants(cb, 0, offset 0, size, data)` (10 000 calls per run) was unimplemented; it is now an extra that only logs (`NX_PUSHC_LOG=1`) until the c[0]
  layout of translated NDVE programs is known. `SetComputeOverlapMode` and `SetComputeWorkloadPriority` are no-ops (they were 22 000 [RED] lines per run).
- The first save-data queries (`caption.sav` for 12 slots, `account.sav`, `option.sav`) answer path-not-found, as expected for a fresh save.
- The logo texture shows vertical streaks: its block-linear layout (or BC decode) is not the Switch 1 one.

## The UI text and glow passes (2026-11-15, later still)
`NX_SMALL_LOG=1` (nvn_draw_vk.c) lists the first 80 draws into offscreen targets narrower than 800 pixels, with the frame, the keys and every bound texture that is not the placeholder
(`NX_SMALL_TRACE=1` also requests the detailed trace two frames on, with `NX_TRACE_SIGNAL=1 NX_FRAME_DUMP=<prefix>`; with `NX_FRAME_DUMP` the first draw dumps the textures it samples).
- They start once, around frame 1250 (about 55 s after the start), and are one chain of the same shader pair (`e1d6b02b` + `8004e4d5`): a 832x640 ASTC atlas and a 256x46 R8 mask go into
  242x40 / 306x104 / 285x94 / 592x49 / 656x113 targets, ping-ponging between an R8 target and an RGBA one (a blur / glow). The R8 targets show as solid red in a PPM (only R is set):
  that is a mask of ones, not a colour bug.
- It is not deterministic: some runs build the chain with different textures bound, and some never get to it within 70 s.
- The ASTC 4x4 logo textures (0x87, `tex5119`, `tex5285`) decode with a regular comb pattern along the black outline. The software decoder's black blocks there are real
  (LA endpoints, luminance 0, alpha 2..6: transparent black), so the comb is probably the alpha channel that the RGB dump hides, not a decoder error.

## The movie is on the window (2026-11-15, end of day)
What got the title movie (Link on Epona, Hyrule castle) onto the window: three things.
1. **Compute programs have a 0x50-byte header, not 0x80** (`ounce_sass.py`: SPH type 12 or 14). With 0x80 every compute program lost its first three instructions, which are where it reads its
   thread and group ids, so the 479 compute programs translated earlier have to be disassembled again. Type 12 is a compute program too (the movie's first and last programs).
2. **The library lacked a vertex shader the title builds itself** (hash `2a1abddfe116d181`, in the `NX_SHADER_DUMP_HASHED` dump but in none of the archives), so about ten draws a frame were skipped:
   the ones that composite the movie. `ounce_hashed` (2,776 programs the title built, 2,503 translate) is now in the library as well (143 new programs).
3. **The video enhancement (NDVE) is not run, a stand-in is**: `tools/ndve_bypass/a22_bypass.comp` replaces the last program (`a22eb9ec`) with a bilinear upscale of the first program's picture,
   and the nine tensor-core programs between them are left out of the library (so their dispatches are skipped).

NDVE itself (Movie/Ndve, 11 programs, 2,216 `HMMA.16816.F16`): all 11 translate and pass `spirv-val` (`Sm86Ndve.cs`: `HMMA` by warp shuffles, `LDSM`, `MOVM`, packed half math, `F2FP`, `SUST`, `TEX.F16`,
`IMAD.X/HI/IADD`, `IABS`, `LEA.HI.SX32`, `.X16`), and the first program runs and gives the right picture (the YUV->RGB of the decoded frame; `PreloadTable` in `Sm86Translator.cs`).
The other nine hang the GPU (`ring gfx timeout` with page faults at addresses near 0): **a compute program starts with registers the launch has filled** (it reads R0.., UR4..UR6 before writing them:
`UR6` is the work group x id (3facac13 divides it by 60), `UR5`/`UR4` presumably y and z, the thread x id often `R4`/`R5`), and the translated code runs with undefined values. The
mapping is known for the first program only (`OUNCE_INIT="R0=ctax;R1=ctay;R5=tidx;R2=1920;R3=1080"`; the picture size in R2/R3 is hard-wired). To do: find it for the others (the checks:
a program whose first instructions use a register no instruction wrote), then `NX_CS_ONLY=<key prefix>` runs one program, `OUNCE_CUT=<hex address>`/`OUNCE_CUTREG=<n>` truncates one and stores a register, `NX_SPV_DIR=<dir>` swaps a program's SPIR-V.

Compute `c[0]` on the host (`driver_buffer`, `nvn_draw_vk.c`), as the NDVE programs use it: pushed constants (`nvnCommandBufferPushComputeConstants`, `OP_PUSH_CC`) from byte 0x10; the storage buffer rows (address,
size, 0x10 each) follow the push block rounded up to 0x40; then the handle table, 8 bytes per entry: texture slot s at base + 8 s, the images from the first entry after the highest texture slot the program uses.
The base is 0x50 except for `a22eb9ec` (0xa0): `CS_HANDLE_BASE` in `shader_pack.py`.

Debugging hooks added today (all environment variables): `NX_TRACE_ADDR` (kernel.c), `NX_PUSHC_LOG`, `NX_SMALL_LOG`/`NX_SMALL_TRACE`, `NX_COPY_LOG`, `NX_CS_ONLY`, `NX_CS_SKIP`, `NX_CS_MAXGROUPS`, `NX_CS_C0LOG`,
`NX_CS_BINDLOG`, `NX_C0_OWN`, `NX_SPV_DIR`, `NX_CODEC_DUMP`, `NX_CODEC_LUMA`. Run with `RADV_DEBUG=hang` on a GPU hang: the report (shader NIR, VM fault page) goes to `~/radv_dumps_*`.

## After the movie (2026-11-15, evening)
The title does not stay stuck: its threads run, the high-priority thread (0x7c) loops on a periodic event, no file is read any more and no draw is skipped (16 per frame: 14 UI quads into
the 2560x1440 UI target, one blit to the window), yet the window keeps the two white bars. What was ruled out: button presses (A, B, +, ZR; `NX_HID_TAP`, `NX_HID_SCRIPT`), a missing shader (the
library has every key the title asks for now), the font service (`pl:u` is never opened). Facts for the next look:
- The UI quads sample real textures (the last movie picture, 683x683 BC noise masks, a 208x160 and a 1664x1280 ASTC logo), so the draws are not the white-placeholder ones of the first analysis; the
  fragment shader `b56e336b` is a glass/dissolve effect that `KILL`s below a small alpha and scales by `c[7]` (a uniform block): an animation parameter that never moves would leave it empty.
- The two bars come from the text chain (`NX_SMALL_LOG`): its R8 masks come out all ones.
- ASTC 4x4 textures show a regular pattern of white blocks (`tex5245`, the logo): a de-swizzle with 2, 4 or 8 GOBs gives the same or worse (`NX_TEX_RAW_WH=<w>,<h>` dumps the raw storage).
New debug aids: `NX_TEX_RAW_WH`, `NX_TRACE_EVENT=<handle>` (kernel.c: creation, signal and clear of an event), and the thread dump lists the handles a `WaitSynchronization` waits on.

## Two more causes found after the movie (2026-11-15, night)
- **A program hash names several builds.** The archives' build of a program and the one the title builds at run time (`NX_SHADER_DUMP_HASHED`, `work/run/ounce_hashed`) share the control hash and differ
  in code (2,378 of the 2,500 dumped programs; the NDVE programs too: the dat file holds two builds of each). The library has to be the run-time builds. The archive build of the UI vertex
  shader `e1d6b02b` never writes its colour outputs (`AST a[0x80]`), the run-time one does: with the archive build every vertex colour and alpha was zero and the logo draw was invisible.
  `pk_all` now links the run-time build wherever the two differ.
- **`@P0 EXIT P1`** is an exit that needs the guard and the predicate operand to hold; the translator ignored the operand and left the shader on every pixel (the logo's alpha test).
Method that found both: `NX_DRAW_TRACE=<frame>` with `NX_DRAW_KEEP=<n,m>` (only those draws are made), `NX_SPV_DIR` with a hand-edited `.sass` for the fragment shader (output the texture, the
alpha, the vertex colour as colours), `NX_PPM_ALPHA=1` for the alpha of a dumped texture.

State at the end of the night: with the run-time builds in the library, the translator's `EXIT Pq` fix and everything retranslated (15,212 of 15,255 archive programs and 2,503 of 2,776 run-time ones),
the screen after the movie is no longer the two bars: the logo sprites (sword, Link's silhouette) and the gold ornaments beside the prompt are drawn, with ASTC artifacts, and three solid
white rectangles remain (one over the logo, two at the bottom). The rectangles are R8 mask targets the text/glass chain leaves at their clear value: its draws are killed because the texture
they sample (slot 8) is the 4x4 placeholder (all zero) in the runs where the title has not got the 832x640 glow atlas bound yet.

## The white rectangles, narrowed down (2026-11-15, night)
`NX_DRAW_KEEP=<n,m>` with the final blit (15) shows which draw paints what on the post-movie screen. Draws 1-3 (VS `9b92e914` + FS `b56e336b`) draw the logo's glow layer (it shows, streaky:
the ASTC artifacts); draw 4 (`e1d6b02b` + `84bed4c4`) is the logo itself and draws right with the fixes above; **draw 5 (`e1d6b02b` + `8004e4d5`) paints the big white rectangle**: that fragment shader outputs the
constant colour `c[8][0..8]` with alpha = the red channel of the bound texture (here the 1464x640 gold-foil texture, whose red is 255 almost everywhere), and `c[8]` (uniform slot 5) holds (1,1,1)
at an address inside the ring that also holds the previous draw's instance data. Whether the title meant that slot to be rebound, or the rectangle is clipped by something the host does not do (a stencil,
a scissor, a depth state), is not known. The two bars at the bottom are draws 9-14 with the same shader. Other findings: BindProgram binds a whole program (`stages 0xdf`/`0xff`, e.g. `e1d6b02b 8004e4d5`),
so the keys of the pair are exact; `NX_BINDPROG_LOG=<from>:<to>`, `NX_TEXBIND_LOG=<stage>:<slot>:<from>:<to>`, `NX_DRAW_SSBO6=1` list what the draw path sees; the logo-sized glass chain
(`NX_SMALL_LOG`) does bind the glow atlas at slot 8 (an earlier reading that it sampled the zero placeholder was a truncated log line).

## The title screen (2026-11-15, end): keys by code hash
The white rectangles were a **key collision**: one control hash names several builds of a program (the title's UI quad shader `8004e4d5` exists as a textured and an untextured variant with the same hash), and the library could
hold only one, so the draws of the other variant ran the wrong code. Ounce programs are now keyed by the SHA-256 of their code area (`keys_hash` in hle_nvn.c; `NX_OUNCE_KEY_HASH=1` brings back the control hash). The library
is built from `work/run/ounce_code` (what the title builds: `NX_SHADER_DUMP=<dir> NX_SHADER_DUMP_HASHED=1`, named by code hash, translate with ounce_sass + ounce_xlate) plus the archives' programs re-keyed by the hash of their
own code (`pk_code`; the script that links them is in the history of this section: runtime builds first, archive builds under `sha256(<key>.code)[:16]`, the nine NDVE network programs left out and `a22eb9ec`
replaced by the stand-in). Result: after the movie the window shows the Ocarina of Time title logo (gold letters, shield, sword) with a shimmer animation and the prompt's gold ornaments; the prompt text is not drawn
and the ASTC textures show stripes. Button presses (A, +) did not move it on (see the next section: that was a host bug).

## Buttons, ASTC memory, mesh shaders (2026-11-15, late)
- **Buttons did not arrive** because a comment in `hid_input.c` had swallowed the `memcpy` of the button mask into the npad state record (the line was `... (consecutive) memcpy(rec + 0x10, &buttons, 8);`). Fixed;
  A (`NX_HID_TAP=0:6`, or `NX_HID_SCRIPT="<s>:0,..."`, seconds counted from the hid block's mapping) now ends the movie early and moves the title on: a second video (Link and Epona by a lake), then a black
  screen drawn with 2 render passes at about 36 fps (the world is not drawn yet, see mesh shaders).
- **GPU memory ran out** (`radv/amdgpu: Not enough memory for command submission`, `vkQueueSubmit2 failed (-4)`; GTT 7.9 GB, the Radeon 660M has 512 MB of VRAM) once the title started creating the world's
  textures: 1,300 ASTC textures are created (expanded to RGBA8, 4-9 times the data: 6.5 GB, many of 3072x3072 = 48 MB) and fewer than 128 had been sampled. ASTC images are now **lazy**: the `VkImage` is made
  at TextureInitialize, its memory is bound by `use()` the first time a draw needs it (`bind_lazy` in nvn_tex_vk.c); GTT stays near 1.2 GB. `NX_TEX_EAGER=1` brings back the old behaviour,
  `NX_TEX_MEM_LOG=1` prints image memory by kind and each image of 16 MB or more, and vkctx.c reports live device memory by allocation site at each gigabyte. Not done: an eviction of idle images, and
  transcoding ASTC to BC (when the world samples hundreds of them the 4-9x expansion will matter again).
- **Sparse virtual pools**: an upload now probes the texture's storage with `process_vm_readv` (`range_readable`) and waits if a page is not mapped (a host SIGSEGV in `nvn_deswizzle` was seen once, in about
  one run in four, and has not come back in twelve runs since; dynarmic's handler now also prints the faulting address and a backtrace when it is not a JIT fault).
- **Mesh shaders**: after the press the title calls `nvnCommandBufferDrawMeshTasks(0, 12)` (and looks up the Indirect and MultiDraw forms). They are registered as logged no-ops (`x_DrawMeshTasks` in
  hle_nvn.c). Drawing the world needs task/mesh stages in the SM86 front end and `VK_EXT_mesh_shader` (RADV supports it on this GPU): the next large item.

## What "mesh shaders" turned out to be, and the real blockers (2026-11-16)
The `DrawMeshTasks` calls (about 500, at presents 2081-2489) are the subtitle/UI layer drawn over the second movie, not the world; the world needs no mesh stage yet (39 programs of SPH type 3 with
word0 & 0x02000000 do use ISBEWR/ISBERD and will need one later; type-1 programs with that flag are ordinary vertex shaders). What stopped the title after the movies was elsewhere:
- **The end of a movie was never reported.** After the last packet (`submit 0 bytes, last 1`) the picture decoded last must carry the `last` flag; without it the movie player waited for ever on a black
  screen. `hle_codec_video.c` now flags the final picture (`d->eof`), the title then flushes and finalizes the decoder and loads the game.
- **prepo:u 10500** (system 20+) returns an object (the title calls its command 0); answering with no object made the next request name a garbage domain id and the title abort. It now returns a
  permissive object (`prepo_obj`).
- **A compute program has no header.** Its code area is instructions from byte 0 (S2R of the thread and group ids, a TXQ, ...). The "0x50-byte header" of earlier notes was five real instructions,
  and the "registers the launch fills" (R0=ctaid.x, R5=tid.x, R2=1920, R3=1080, the UR registers of the network programs) were that prologue, dropped. A graphics program starts with an SPH whose
  word 0 & 0x3ff is 0x81 (vertex, mesh) or 0x82 (fragment); everything else is compute (ounce_sass.py, Program.cs and Sm86Translator.cs now agree; `PreloadTable` is empty).
  The compute program that reads `c[0]` through `LDG [UR10]` and faulted at address 0 was `fe5eaf48...`: UR10 was never set because its prologue was gone.
- **TXQ** wrote its width into R2 and then used R2 as the level of the height query (`TXQ RZ, R2, R2`): the level is copied first now. With that the YUV-to-RGB program (`dd8b3c1d`) gives the right
  picture, and the title movie shows in colour again.
- **NDVE**: the network programs now translate with their prologue, but the last one (`893e77ef`, 5400 groups, 4 bindings, tensor-core loop) still hangs the GPU. The pack leaves the nine network programs
  out and uses the bilinear stand-in for `a22eb9ec` (`tools/ndve_bypass`), as before.
- `tools/ounce_pack_dir.py OUT RUNTIME ARCHIVE` assembles the pack directory (run-time programs plus archive programs re-keyed by the SHA-256 of their code), then `tools/shader_pack.py`.
- **The Vulkan validation layer is off** unless `NX_VK_VALIDATION=1`: it crashed inside itself on the title's draws and made everything several times slower.
- **ASTC is held as BC3** (`NX_ASTC_RGBA8=1` for RGBA8): upload decodes the blocks and `src/gfx/tex/bc3enc.c` encodes them (bounding-box endpoints, nearest palette entry), 1 byte a pixel instead of 4.
  Image memory in the loading phase fell from 7.3 GB to about 4.1 GB live (GTT 4.7 GB); the machine (15 GB RAM, the GPU shares it) is still very tight with `NX_HEAP_MB=12000`.
- **Compute `c[0]` layout (corrected 2026-11-16)**: pushed constants from byte 0x10, the handle table at 0x50, and the storage buffer rows (address 64 bits, size 32 bits, 0x10 each) at a FIXED
  0xc0 (`driver_buffer`, `nvn_draw_vk.c`). The old rule (rows after the pushed block, rounded to 0x40) put them at 0x10 for a dispatch with no pushes; the 609 particle programs of the world (all
  `_Z1kPf`, ~190 KB of SASS each) read `c[0][0xc0]`, `c[0][0xd0]`, `c[0][0x150]`, `c[0][0x160]`... as pointers (slot (offset-0xc0)/0x10), so they read address 0 and hung the GPU. That fault is gone.
  An address that lies in no memory pool (`nvn_vk_device_address` returns 0) is replaced by a page of zeros (logged as "is in no memory pool").
- **Open (2026-11-16)**: with that fixed, the world load still ends in a GPUVM fault, now a *graphics* draw (SQC data = a scalar load; RADV reports the VM page as a number whose scale I could not tie to
  the binding report). RADV's hang dump gives the pipeline of the last draw: its fragment shader is byte-for-byte the size of `0da6249b...` and its vertex shader a 50.6 KB program (`18dce6fc`, 50636
  bytes, or `5f4ac39c`, 50436). Those vertex programs fetch their vertices through `c[0][0x120]` (storage row 1, a 5.7 MB buffer) at `(idx + c[5][0x2ec]) * 0x20`, with `idx` packed in `c[4][0x88]`:
  a garbage `idx` in the uniform buffers (they are read by the GPU later than the title wrote them, if the host does not snapshot them) would read past the end of the pool and fault.
  Not checked yet: whether the uniform buffer contents seen by the GPU are the ones of the draw's time.
- **Hang hunting hooks (all env)**: `NX_SUBMIT_SYNC=1` (wait for the queue after each submit; RADV cancels the submit AFTER the one that faulted, so the guilty one is the previous: it prints that
  submit's last shaders), `NX_CMD_DUMP=<path>` (every submit's shader keys, vertex and fragment per draw and the key of each dispatch, written to `<path>.0`/`.1` alternately BEFORE the GPU runs
  it, because `RADV_DEBUG=hang` aborts the process), `NX_VS_LOG=<vertex key prefixes>` (every draw with that vertex shader: index in the submit, counts, the stage's storage rows).
  RADV's `trace.log` gives the first trace point the CP never reached (here id 8697, with 4059 draw packets before it, the same in every run); the draw index in the submit is close to but not equal to
  that count (RADV adds its own draws), so match pipelines by the dumped SPIR-V sizes rather than by index. The box needs about 9 GB of free RAM for a run: the kernel OOM-killed two runs when it had less.
- **Which pipeline faults (2026-11-16, later)**: `NX_SPV_LOG=1` prints each module's key and SPIR-V byte size; the RADV dump's two `.spv` files match `18dce6fc000c2d7d` (vertex; 50636 bytes plus the 16 the
  invariant-position patch adds = 50652) and `0da6249ba5401be1` (fragment, 48324). That vertex program samples textures with handles it loads from the storage row 1 table (`TEX.SCR.LZ ... UR8`, a
  32-entry array indexed by `min(ID, 31)`). Checked and NOT the cause: the vertex fetch indices (`NX_VS_LOG` shows all in range), the uniform buffers being rewritten before the GPU runs (a copy per draw,
  `ubo_snapshot`, is on for Switch 2 titles; `NX_UBO_LIVE=1` turns it off; the fault stayed), the by-key debug state (`g_dbg_keys` was stale for Switch 2 programs until the lookup by code; fixed).
  `NX_VS_SKIP=<vertex key prefixes>` drops those draws; the fault stayed with `18dce6fc` skipped, so either another pipeline faults too or the skip did not match: unresolved. Suspects left: descriptor
  arrays filled with image views of lazily bound ASTC textures, and image descriptors without `nullDescriptor` (the device is created without VK_EXT_robustness2).
- **Machine state**: after two OOM kills an `nxlinux` thread stayed in state D inside amdgpu (`drm_suballoc_insert`), pinning 4.2 GB of GTT; the process is a zombie that cannot be killed. Only a reboot
  (or a GPU reset as root) frees it, and until then a run has about 4 GB less to work with and is OOM-killed.
- **Hang hunt, second round (2026-11-16, after the reboot)**: the same pair (`18dce6fc000c2d7d` + `0da6249ba5401be1`) is the pipeline of the last draw in the RADV dump, run after run. All of these were
  tried and the fault stayed: skipping `18dce6fc` (it moves to `5f4ac39c` + the same fragment shader), skipping the fragment shader `0da6249b` (it moves to `3fa32c11` + `f1acebca`, the same 48-50 KB class),
  skipping every draw of a shader of 40 KB or more (`NX_SKIP_BIG=40000`: the dump then names a small compute program, `91e5d7fe3d445bb3`, which stores through `c[0][0x20]` while its c[0] is all zeros),
  `NX_TEX_EAGER=1`, `VK_EXT_robustness2` nullDescriptor (now enabled in vkctx.c, `NX_NO_NULLDESC=1` turns it off), a per-draw copy of the uniform buffers, translating pushed 64-bit pool addresses to device
  addresses (none were ever pushed), and filling the compute storage rows at the old position too (after the push block) as well as at 0xc0.
  Measured: the fetch indices of both programs (vertex: `(c4[0x88] & 0xfffff) + c5[0x2ec]`, fragment: the same from `c4[0x84]`) stay below 600 KB of the 5.7 MB table in every logged draw; the fragment stage's
  storage rows (`S.ssbo[1]`, NVN stage 1 is fragment) equal the vertex stage's. The RADV report's "Failing VM page" is not the VA (it does not fall in any bound range however it is scaled).
  Unexplored: what the fault address belongs to (run with `RADV_DEBUG=allbos` for `bo_ranges.log`, and `NX_C0_OWN=1`), the `ALD`/`IPA` attribute path of these programs, and the second storage buffer
  of the vertex program being written by an earlier compute dispatch the title expects to have finished.
- **Host fix**: the HID pump thread (`hid_input.c`) wrote the shared-memory block after the title had freed it (SIGSEGV at frame ~545 of the world load, `rip` in `pump`); it now probes the 64 pages with
  `process_vm_readv` each pass and drops the mapping if one is gone.
- New env switches (all debugging): `NX_SPV_LOG`, `NX_VS_LOG`, `NX_VS_SKIP` (vertex or fragment key prefixes), `NX_SKIP_BIG=<bytes>`, `NX_UBO_LIVE`, `NX_PUSHC_RAW`, `NX_CS_ONE_TABLE`, `NX_NO_NULLDESC`.
- **Third round (2026-11-16)**: `RADV_DEBUG=hang,allbos` writes `bo_ranges.log` (867 buffer objects, 0x800000000000 up). The "Failing VM page" printed by RADV, shifted left by 8, falls inside a 64 MB image
  memory block (`0x80017b600000-0x80017f600000` in that run), the only scale at which it lands in a mapped range; whether that is the real address is not established. Tested and rejected: a guest unmap of
  memory the GPU imported by host pointer (`NX_MEM_UNMAP_LOG=1` logs each unmap of 64 KB or more and whether it overlaps a pool: 916 unmaps, none in a GPU pool). Still open; see the list of
  suspects above. `nvn_vk_pool_overlap` (nvn_vk.c) and `unmap_note` (mem.c) stay as debugging aids.
- **Fourth round**: empty (or unmapped) storage rows of a Switch 2 shader now point at a shared 64 MB zero buffer (`dummy_addr`, `nvn_draw_vk.c`; `NX_NO_DUMMY=1` turns it off), compute rows 0-3 of the
  post-push table included. The fault stayed, at the same frame. Runs with `NX_SUBMIT_SYNC=1` (the GPU finishes each submit before the next is recorded) fault as well, so a buffer freed or invalidated while
  a submit is in flight is not the cause either: the fault is inside one submit and deterministic. What is left is a stray address the shaders compute themselves (not through the tables), or a
  translation fault in one of the 48-50 KB programs; a per-access bounds check in the translated SPIR-V (every `LDG`/`STG`, against the bound rows) would make it harmless but needs a retranslation and repack
  of ~19,000 programs.
- **Two real bugs found with the validation layer (2026-11-16, fifth round)**; the GPU fault itself is still there after both.
  1. *Binding collision.* The translator numbers a stage's bindings in order from 1 (vertex) or 64 (fragment) and a 32-element texture array takes 32 numbers, so a vertex program with arrays reached 66-69 and met
     the fragment program's textures from 64 on (VUID-VkGraphicsPipelineCreateInfo-layout-07990: `fp_t_cb2_C` a texture where the set layout has a uniform buffer). Fragment shaders of a Switch 2 title are now moved up
     by 1000 (`ensure_shift`, nvn_shader_vk.c, in both the reflection and the SPIR-V; `NX_NO_BIND_SHIFT=1` turns it off), so no retranslation was needed.
  2. *The 32-element texture and sampler arrays were never written.* `array_fill` (nvn_draw_vk.c) existed and nothing called it, so those bindings kept whatever the pooled set held (VUID-vkCmdDrawIndexed-None-08114).
     They are filled per draw now (up to eight arrays).
  Both are general to any title whose shaders index textures by pool ID. After them the validation layer reports only `Input-08733` (a float vertex input fed by an `R8G8B8A8_UINT` attribute), `OpEntryPoint-08743`
  (a fragment input with no vertex output) and `pNext-02662` (an image view asking for storage usage the image lacks).
  Still failing: the first draw of vertex `18dce6fc000c2d7d` + fragment `0da6249ba5401be1` (count 327, base vertex 3525, the 5.5 MB storage pool `0x309f898000`), found by `NX_DRAW_SYNC_FROM=<present>`
  (every draw printed, submitted and waited for from that present; the last line before the fault is the culprit) and reproduced after both fixes; ablating that family's pointer loads or its texture samples in the SPIR-V
  (`NX_SPV_DIR`) does not change it, and GPU-assisted validation (`VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT`) reports nothing before the fault.
  Next: the format mismatch at vertex location 6 (`R8G8B8A8_UINT` against a float input) and the missing vertex outputs for the fragment input at location 5 are the only things left that differ between this pair and
  draws that run; `pNext-02662` is a bug in `nvn_tex_vk.c` (view usage) worth fixing on its own.
- **Range validation (sixth round)**: for Switch 2 titles each draw's vertex index range (the index buffer scanned, plus base vertex) is checked against the bound size of every vertex stream its attributes
  read (`range_ok`, nvn_draw_vk.c; instance-rate, zero-stride and size-0 streams are not checked; `NX_NO_RANGE_CHECK=1` turns it off). A draw that reaches past a stream is dropped with a one-line `[RED]` note.
  It dropped at least one draw in the world load, and the GPU fault stayed, so the draw that faults (`18dce6fc` + `0da6249b`, first draw, 327 indices) is inside its streams: the cause is not an index overrun.
- **Seventh round: the hang is not RADV's** (2026-11-16). `NX_GPU=1` runs the title on the NVIDIA GeForce RTX 2050 (proprietary driver, `vkEnumeratePhysicalDevices` order: 0 = Radeon 660M, 1 = RTX 2050).
  It stops at the same place of the log (after `device memory: ... MB live`, `draw skipped: indirect dispatches are not supported yet`, `draw skipped: a compute shader needs what the dispatch path cannot supply yet
  (needs 0x10)`, `stand-in #1: texture id 0 is not registered`): `vk: submit or wait failed (2)` (VK_TIMEOUT, the 10 s fence wait) and NVRM channel timeouts in the kernel log. Two drivers hang on the same
  submission, so the work we submit is wrong, not one compiler. Tested and rejected on the way: attribute integer-to-float (`NX_ATTR_INT2F=1`), pools held as private copies instead of host-pointer imports
  (`NX_POOL_COPY=<min bytes>`), RADV's LLVM backend (absent from this build), range validation of vertex indices (`range_ok`), dummy rows for empty storage tables.
  The two compute warnings right before the hang are the lead now: a compute program skipped for `needs 0x10` (a texture binding the packer could not classify, `NEEDS_TEX`) and an indirect dispatch (unsupported)
  are not producing data the next draws read; the 5.5 MB storage table (`0x309f898000`) the culprit draw indexes is a likely product of them.

## The GPU hang: missing compute programs, then raw pointers (2026-11-16, round 8)
- **Both GPUs hung on the same submission** (RADV GPUVM fault, NVIDIA channel timeout), so the work was wrong, not the driver. The log said a compute program was skipped (`needs 0x10`) and indirect dispatches were
  not supported; `NX_SKIP_LOG` then showed 64 distinct compute programs the title dispatches that the library lacked. They were missing because the Sm86 frontend could not translate them, not because they were not dumped.
- **What was added** (all in `Translation/Sm86` unless noted): `LDC.U8/U16/S8`, `SHF.*.W`, `BREV`, `VOTE`/`VOTEU` with a ballot register, `REDUX.SUM/MIN/MAX` (new IR `ReduceAdd`...`ReduceMinU32`, SPIR-V group
  arithmetic, capability declared only when used), `R2P`, `S2R SR_LTMASK` and the other lane masks, `RED`/`ATOMG` on raw addresses (atomics on `PhysicalMemory`; `RED.E.ADD.F32` as `AtomicAddF32` with
  `VK_EXT_shader_atomic_float`, enabled in vkctx.c), `IADD3` with two carry outs, `SULD.P`/`SUST.P` 2D and 3D, `RET.REL.NODEC` (returns resolved from the `MOV Rk, <address after call>` of the calls to the enclosing function),
  `VOTEU URd, UPd, Pa`. The host now runs **indirect dispatches** (`vkCmdDispatchIndirect`).
- **Two translator bugs that hid programs**: SassText.Parse recognised only four-digit addresses, so any program of more than 4096 instructions was cut at 0x10000 (the labels beyond it were "used but not defined" and
  GotoElimination threw a NullReferenceException); and the cut at the compiler's trailing `BRA self` dropped the subroutines placed after it (now kept when something before it branches or calls past it).
- **A dump bug**: a program the title builds itself has 0 at control +0x30, so the dumped `.ctl` stopped at 0xe0 and its BRX jump tables were lost. The table is at +0xd8 (offset) and +0xdc (length); hle_nvn.c now dumps it.
- **Still untranslated** (8 of the 64): `TLD.SCR.LZ`, `TLD4.SCR`, `SURED.D.2D`, `HSETP2`, `F2IP.U8.F32`, `BRXU`, one program with a `RET.REL` form not matched; the nine NDVE network programs stay out on purpose.
- **Library rebuild**: `NX_SHADER_DUMP=<dir>` run -> `ounce_sass.py` -> `ounce_xlate --dir` on the dump and on `work/run/ounce_all` -> `ounce_pack_dir.py pk_code3 <dump> ounce_all` -> `shader_pack.py out.nxsl pk_code3`
  (18,590 programs). `ounce_xlate --dir` exits non-zero when any program fails: do not run it under `set -e`.
- **Debug switches added**: `NX_CS_SYNC=<present>` prints (key, groups, indirect address, storage and uniform rows, pushed constants, the memset count word) and waits after every dispatch from that frame on; combine it
  with `NX_DRAW_SYNC_FROM=<present>` to see draws and dispatches in one order (the submission a wait covers includes the draws recorded since the last cut, so a dispatch alone can be blamed wrongly).
- **Result**: with the new library the title gets much further (present 2182, 5500 synced draws instead of dying in the first submission of the world) and the fault moves to `afb4ddcc` (an indirect compute dispatch, 492 KB of SPIR-V).
- **The root cause now visible**: the faulting address, `0x318bf91000`, is a *guest* address in the same pool as that dispatch's `ssbo2`/`ssbo3` (`0x318bf91940`, `0x318bf91880`). A shader that follows a 64-bit pointer **the game
  stored in memory** reads an NVN GPU address, and in this port the GPU address of a pool is the guest pointer while the device address is something else (`0xffff800100052000`). The c[0] rows are translated
  (`nvn_vk_device_address`), pointers inside the game's own data are not, and cannot be found. This also explains the earlier faults at `0x309f898000`+ and in the 50 KB graphics pairs.
- **Probe** (scratchpad, `va.c`): `bufferDeviceAddressCaptureReplay` is available on RADV and on the NVIDIA driver, but RADV only honours a requested address inside its own range (`0xffff8000_00000000` upward;
  `0x31...` gives `VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS`), so the guest address cannot be reproduced on the device.
- **The fix that follows**: make the GPU addresses the game sees *be* the device addresses: `nvnMemoryPoolGetBufferAddress`, `nvnBufferGetAddress`, `nvnTextureGetAddress`... return the pool's device address + offset, every
  place that looks a GPU address up (`pool_find` in nvn_vk.c, `nvn_vk_pool_buffer`, the draw path, replay) maps it back through a device-address table, and pools get their import at initialisation (a virtual pool
  keeps the guest address, or a sparse buffer reserves its range). Switch 2 titles only at first; Smash and MK8D keep the identity convention.
- **Done: Switch 2 titles are given device addresses.** `nvn_gpu_addr_out` (nvn_vk.c) turns a guest address in a pool into `pool device address + offset`, importing the pool at that moment (only pools whose address the title asks for are imported: `nvnMemoryPoolGetBufferAddress`, `nvnBufferGetAddress`, `nvnTextureGetTextureAddress`); `nvn_gpu_addr_in` maps a device address back through a table of the imported pools sorted by address. hle_nvn.c converts where an address crosses the NVN API (`GOUT` / `GIN`: every bind, draw, indirect, copy, report, clear and transform-feedback entry, the buffer ranges, and the shader code address), so the rest of the layer still works with guest pointers. `in` leaves anything below `0xffff8000_00000000` alone, so converting twice is harmless. `NX_HOST_ADDRS=1` restores the old convention for a Switch 2 title. Probe `vb.c`: a host-pointer import ignores the requested capture-replay address on RADV (and NVIDIA refuses it), so the title cannot be given addresses chosen in advance, only the ones the import returns.
- **Result**: the first world submission no longer hangs the GPU and the title passes frame 2182 (where `afb4ddcc` faulted at `0x318bf91000`) without a `GPUVM` fault.
- **Round 9 (what stopped the next hangs).** With device addresses the title gets past present 2182 and the next failures were of other kinds, found one by one with `NX_SUBMIT_SYNC`, `NX_DRAW_SYNC_FROM`, `NX_CS_SYNC` (+ the new `NX_CS_SYNC_DATA=1`, which prints the first words of every storage and uniform row, the indirect arguments, and a dereferenced view of any uniform that is a table of device addresses) and `RADV_DEBUG=hang` (the dump's `.spv` size identifies the program):
  - **Compute programs with sampled textures** were skipped (`needs 0x10`): `cs_handle_slots` in `tools/shader_pack.py` accepted only plain 2D (`type == 3`); it now takes 2D, 3D and cube with the array (256) and shadow (1024) bits too. 100 more compute programs run (needs-TEX went from 108 to 8).
  - **Reads through a null pointer.** `4dc886e0` picks a level from thresholds and reads a table row; the levels that are empty have a row of 0, and the program then loads from `0 + offset`. The console's GPU lets that through (a read of zero), a PC GPU faults and loses the context. The translator now guards every raw load, store and atomic (`GenerateLoadOrStore`, `GenerateAtomicMemoryBinary`, `GenerateAtomicAddF32`, helper `GuardedPhysical`): an address whose high word is 0 (anything below 4 GB) gives 0 and writes nothing. Operands are evaluated before the branch, because an operand may itself hold a guarded load and open blocks.
  - **`24ff13bd` was the compute `c[0]` layout, not the shader**: see "Round 10".
  - The title also aborts by itself in about a third of the runs, right after `WindowInitialize` (`main+0xeb6838`); it did before these changes too (a run died at frame 2 earlier). Re-run.
  - Diagnostics added: `NX_POOL_LOG2=1` (every pool made and dropped, with the present number).
- **Round 10 (the hangs of `24ff13bd`, `c409e952` and `4dc886e0`: wrong storage rows in c[0]).** Their signature in RADV's dump: `SPI_BUSY` and nothing else busy (no TA, no TCP, no UTCL2), trace point of the dispatch reached and the next one not, no `GPUVM fault`: the waves
  stall on a load or store through an address that is canonical but mapped nowhere (a replayable page fault that never gets served). Bisecting a program's SPIR-V (`spirv-dis`, edit, `spirv-as --target-env spv1.3`, `NX_SHADERS_EXTRA=overlay.nxsl`) showed it was
  not the loop, the guards, the texture samples or the atomics; the loads came from rows that held the wrong buffers.
  - **The layout.** The Ounce control section's dword at **+0xb0** says how a compute program's c[0] is laid out: **byte 1 = the pushed-constant block in 16 byte units** (it starts at `c[0][0x10]`) and **byte 2 = the number of storage rows** (address 64 bits, size 32 bits,
    0x10 each) that follow it; the handle table of textures and images comes after the rows. `24ff13bd` is 3+5 (rows from 0x40, textures at 0x90), `c409e952` 3+3 (textures at 0x70), `4dc886e0` 4+4 (rows from 0x50, textures at 0x90), `7d3f806b` 1+5 (rows from 0x20),
    `afb4ddcc` 3+10: each matches the offsets the SASS reads. Row r is the NVN storage buffer r. The old rules (rows at a fixed 0xc0, or after the pushed block rounded to 0x40) were right for neither: `24ff13bd` read `c[0x60]` = the start of `ssbo5`,
    `0xffffffff00000000`, a canonical address nobody maps.
  - **Host** (`driver_buffer`, nvn_draw_vk.c): for a Switch 2 compute program whose byte 1 is 1..7 the rows are written at `0x10 + 0x10 * units` (unbound rows below the row count point at the zero page), and the region above is cleared first. `NX_CS_OLD_ROWS=1` keeps the old fills.
    **Packer** (`shader_pack.py`, `cs_ctl_layout`): the handle table base of such a program is `0x10 + 0x10 * (units + rows)` (it was 0x50 for all, so `24ff13bd`'s textures were bound to slots 8 and 9, now 0 and 1); `ounce_pack_dir.py` links the `.ctl` beside each program.
  - **Not changed**: programs with byte 1 = 0 (NDVE) and the particle programs with byte 1 = 12 or 13 (`717d27d4`: 0xb0 bytes of push block would put the rows at 0xd0, and `c[0xc0]` would be a pushed value; the fixed 0xc0 rule has not faulted for them, so it stays until a
    dump shows otherwise). The dispatch dimensions are written at 0x200 only; `c409e952` reads `c[0][4]` and `c[0][8]` (probably the grid), which stay 0.
  - **Result**: with the new library (`work/run/ounce_all5.nxsl`) the title runs `4dc886e0`, `24ff13bd` and `c409e952` at present ~2144 without a hang, and keeps going (4074 synced dispatches after present 2144 with `NX_CS_SYNC=1900`, no `GPUVM`, no `failed (-4)`). The frames are still black after the loading icon.
  - **Tools**: `NX_SHADERS_EXTRA=overlay.nxsl` (a second library whose programs replace those with the same key: to try one edited program without repacking 2.9 GB), `NX_POOL_MAPS=1` (how the host's mappings cover each imported pool of 1 MB or more), `NX_CS_SYNC_DATA=1` now also prints each storage row's pool and whether the row is cut by the pool's end, resolves device addresses stored in the rows, and scans `24ff13bd`'s index words.
- **Round 10b (libraries and a regression).** `ounce_all6.nxsl` (round 10 + the NDVE stand-in) draws nothing: it lacks 851 programs (426 vertex, 424 pixel, 1 compute) that `ounce_all2.nxsl` had, from dumps the later pack directories did not include (`ounce_hashed`, `ounce_own3`...). A library that works is
  **`work/run/ounce_all7.nxsl`** = all6 plus those 851 (19,441 programs; built by merging, see `tools/run_oot.sh`). Bisected with `NX_SHADERS_EXTRA` (a main library plus an overlay): all2 + all of all6, all6 + all2: both draw; all6 alone: black. The version of `a22eb9ec` matters too: the real translated
  NDVE last stage renders noise (vertical dotted columns), the stand-in `work/run/ndve_bypass` is right (`ounce_pack_dir.py OUT RUNTIME ARCHIVE STANDIN_DIR` links it over the translated one).
  With all7 the title shows the intro movie (Ganondorf) and the loading screen (black with the triforce in the corner) and no GPU fault; the kernel OOM-kills the process during the world load at frame ~1900-2100 (3.8 GB of anonymous memory plus 4.3 GB of device memory, `NX_DEAD_MB=300` did not help).
- **Regression I introduced and fixed**: the Ounce control-section path in hle_nvn.c keyed on magic `0x98761234`, which is also the magic of the dummy control sections Smash and Mario Kart 8 set (their draws were all skipped: 0 render passes a frame). `ounce_control()` now requires `dm_switch2_gfx`. Check both titles after any change to the NVN layer:
  `NX_PRESENT=null tools/run_smash.sh build-claude` and `tools/run_mk8d.sh` must log "N render passes" with N > 0 in `replay per frame`.
- `tools/run_oot.sh [BUILD_DIR]` starts the title (window by default).



### Round 10c: memory of the world load
- `nvn_tex_vk.c`: a Switch 2 title's ASTC texture of 512 texels or more is held without its top mip level (`NX_ASTC_SKIP=<n>`, default 1, 0 keeps all). The image is a quarter of the size; upload and sampling views shift the levels by `e->skip`. Gated on `dm_switch2_gfx`, so Smash and MK8D are untouched (checked: 7 and 37 render passes, ctest 21/21).
- Effect on Ocarina: ASTC-expanded memory 1.8 GB -> 0.75 GB at frame ~1900; the load now ends (frame ~2210, `OP_RENDER_COND` and `OP_COPY_T2T` appear) before the kernel's memory runs out. Past that, anonymous memory rises from 3.8 to 5.6 GB and device memory from 3.6 to 5 GB in about 15 s (a 12288x12288 target of 297 MB is created); with 15 GB total the run is cut at ~500 MB available.
- `NX_ASTC_SKIP=2` did not change the picture (the kill came at the same frame), so the default stays 1.
- What grew in the last 15 s: a malloc trace (LD_PRELOAD wrapper on malloc/calloc, allocations of 30 MB or more) showed the 112 MB / 224 MB / 32 MB blocks are dynarmic's robin-hash patch tables (`FastmemPatchInfo`, `PatchInformation`), four times, one set per guest core. They grow with the JIT code cache. `src/host/cpu.cpp` now reads `NX_JIT_CACHE_MB` (default dynarmic's 128). With 48, a run lasts the whole 330 s window: past frame 2340, no kill, anonymous memory falls back to 3 GB when the cache is flushed. `tools/run_oot.sh` sets 48.
- Still open: the world frames are black (2 render passes per frame, 0.4 to 2 s a frame, device memory stays at 5.2 GB).

### Round 11: the TEX/TLD operand split (stripes on every movie frame, missing prompt text)
- `Sm86Texture.cs` took the coordinates of a `TEX.SCR` / `TLD.SCR` read as "Ra and Ra+1 first, then Rb". The operands split the sequence (array layer, coordinates, lod or bias, depth reference) in two instead: Ra takes the first ceil(n/2) elements, Rb the rest. A 2D read without a lod (n = 2) is x in Ra and y in Rb, which are rarely consecutive (`TLD.SCR.LZ R11, R2, R1`: x in R2, y in R1; `TEX.SCR RZ, R35, R35, R8`: u in R35, v in R8). With the old reading half the columns of the movie's picture (the NV12 conversion `dd8b3c1d`) read a garbage y: the vertical stripes of every movie frame (period 2, 8 after the 4/3 upscale). Bindless reads are unchanged.
- The fix touches thousands of programs: `ounce_xlate --dir` on ounce_code4, ounce_hashed, ounce_code and ounce_all (about 3 minutes), `rt_merged` (symlinks, first of code4/hashed/code wins) as the run-time dir, `ounce_pack_dir.py pk_code6 rt_merged ounce_all ndve_bypass`, the 18 NDVE network programs removed again (9 archive keys + 9 run-time keys; the first program `dd8b3c1d`/`1fadca19` stays), `shader_pack.py ounce_all8.nxsl pk_code6` (19,673 programs). Result: the movie is clean and bright, and the title menu text ("New Game", "Options / ZELDA NOTES Setting") is drawn.
- Left: the logo's edges still show a halftone comb; the world frames are black (see round 10c).
- Speed: 7 to 15 fps in the intro. Not the GPU (busy 0 to 30 %, clocks drop) and not the movie compute (skipping `dd8b3c1d`,`a22eb9ec` changes nothing): the machine swaps (4 to 11 GB of 15 in swap, 100 to 170 MB/s out while the game runs) and `submit+present` stalls up to 0.5 s with the window off. Scratch libraries in tmpfs `/tmp` (2.5 GB) were part of it. Close other applications; `sudo swapoff -a && sudo swapon -a` brings the swapped pages back.

### The library is mapped, not read
`nvn_vk_shader_load_library` used to read the whole `.nxsl` (3.2 GB for ounce_all8) into memory and copy every program's SPIR-V into its own allocation: 3 GB of cold anonymous memory in the host heap, which the kernel pushed to swap. The swap on this machine is zram (compressed RAM), so that memory cost RAM and CPU either way. The library is now `mmap`ed (`MAP_PRIVATE`, `MADV_RANDOM`) and the shaders point into the mapping (clean page cache, droppable; the fragment-stage binding shift copies only the pages it edits). Measured: the game's own swap went from 4.8 GB to 0.7 GB at 90 s; the frame rate did not change much (8-10 fps in the title), so memory pressure from the GPU's GTT allocations (the iGPU has 428 MB of VRAM, textures live in system RAM) and the other applications remains the limit.

## Round 12: the world frame (black window)
After the world load (frame ~2900 of a run, 9 min on this machine) the game draws 65 render passes and ~1,090 draws per frame, but the window is black except one line of subtitle text with deformed glyphs. Found with `NX_DRAW_TRACE=3300 NX_FRAME_DUMP=<prefix>` (every render target of that frame goes to a PPM; the dump now also writes R11G11B10F targets, and `read_storage` no longer faults on a texture over unmapped storage):
- The frame is a full deferred frame: depth passes (rts 0), hundreds of draws into one HDR target (serial 14962 / 61929, 1920x1080), downsample chain 480x270 .. 30x16 (bloom/blur), a fullscreen triangle into the HDR colour target 57681/61958, then the UI (30-vertex draws, text) into a 2560x1440 target, and one 6-index quad into the swapchain.
- The swapchain ends with only the UI layer: the scene never reaches it. The HDR scene targets are not black but wrong: blocky, over-exposed smears with cyan/magenta casts (not the picture of a world), so two things are open: (1) the shading passes read wrong data (a blocky texture/shadow/lighting input), (2) the last quad (keys 192528a6 / 2f76fc0b) shows black for the scene, either a fade at 0 or an input it samples that is empty.
- The same image is static for 330+ frames (shots identical), so the game is waiting in this state (an intro narration on black?), not crashing.
- Not yet checked: what the final quad samples (NX_DRAW_SERIAL=<serial> shows the draws that sample a texture, but serials change per run), the 8 untranslated compute programs, mesh shader draws (`DrawMeshTasks` is skipped, first at present 552).
- **The last quad is a plain blit of a movie-sized texture** (`2f76fc0b`: one `texture()` read of `c[2][8]`, serial 4337/4323, 2560x1440 R11G11B10F, the size of the NDVE stand-in's output). In the traced world frame nothing draws into it but the UI (the subtitle) and no NDVE dispatch (`dd8b3c1d`) runs: the 3D scene (draws into the 1080p HDR target, post draw `526b2e37` into a 1080p target 63857) is rendered but never reaches the window. The four dav1d workers are idle in this phase (the title movie kept them busy), so the movie engine is waiting, not decoding. Suspect: the audio clock (the Switch 2 voices are 0x188 bytes, `VOICE_IN` is still 0x170 in `audren_mix.c`; `ipc_audren.c` is another agent's) or a movie-engine wait on something else. Not confirmed.
- `ar_mixer_update` took the host down (general protection fault) reading ADPCM coefficients from a garbage guest pointer; the read goes through `process_vm_readv` now.
- **Voice size fixed (0x188 for revision word `REV@`)**: with it the audio clock moves, the world scene advances to the next narration line (frame ~3446 of a run: "My heart feels heavy. Soon I will no longer be able to protect the forest. I must ensure its safety.", clean text, over black), and the mixer no longer takes the host down. The picture behind the subtitle is still black: the movie layer's 2560x1440 target gets no NDVE picture, the 3D scene is not what the last blit shows. Open: why the movie layer has no picture (the NDVE stand-in needs the first program's output; the `dd8b3c1d` dispatch does not run in these frames) and whether this cinematic is meant to show a picture there.

## Round 13: the window target and the per-submission cost
- **`nvnCommandBufferCopyTextureToTexture` was recorded but never replayed** (`OP_COPY_T2T`: "recorded but not executed yet", 2,048+ per run). The world frame renders its scene at 1080p and the last blit (`2f76fc0b`) shows a 2560x1440 target that only the UI draws into: the scene reaches that target through a texture-to-texture copy. `nvn_vk_copy_t2t` (nvn_tex_vk.c) now records it: `vkCmdCopyImage` when the regions and texel sizes match, `vkCmdBlitImage` otherwise (linear with `NVN_COPY_FLAGS_LINEAR_FILTER_BIT`, mirror flags swap the destination corners). `NX_NO_T2T=1` restores the old behaviour. Not yet verified on screen when this was written.
- **Per-submission cost.** `vkQueueSubmit` costs 6-17 ms on the Radeon 660M and grows with the imported pools. Measured with a standalone program (empty command buffer, bufferDeviceAddress on): 1,000 host-pointer imports of 400 KB cost 2.05 ms per submit, one 390 MB import 1.14 ms, 1,000 plain allocations of 400 KB 0.56 ms. amdgpu walks and revalidates the pages of every userptr object in every submission, so the cost follows the imported bytes and, under memory pressure, pages coming back from zram. `NX_POOL_PLACED=1` (needs `VK_EXT_map_memory_placed`, which RADV and the NVIDIA driver have) gives each pool made by `MemoryPoolInitialize` its own device memory (host-visible, cached), mapped over the pool's address range with the game's bytes copied in (`pool_place`, nvn_vk.c), before dirty tracking starts; finalizing the pool copies the bytes back into ordinary memory (`pool_unplace`). Off by default until measured.
- `ubo_snapshot` compares against a shadow copy in cached memory instead of reading the transient arena (uncached device memory on an integrated GPU).
- **Measured (offscreen, 600 s, Radeon 660M)**: `NX_POOL_PLACED=1` with pools of 1 MB or more placed: title 16.8 fps (10.6 before), world 9.45 fps (3.3), frame 6344 reached in 600 s (2954), submit 22/44 ms per frame (55/85), no fault. Placing every pool faulted at the world load: a compute program reads past the end of an 8 KB pool (its storage rows end exactly at the pool's end), which an imported pool hides because other memory follows it; `NX_POOL_PLACED_MIN` (default 1 MB) keeps small pools imported (240 of 4,121 pools, 3,388 of 3,566 MB, are placed). `tools/run_oot.sh` sets `NX_POOL_PLACED=1`. RADV_PERFTEST=localbos, NX_DEAD_MB=256 and NX_SDL_VSYNC=0 changed nothing measurable. Smash (6-7 passes) and MK8D (93 passes) unchanged with the new binary.
- The texture-to-texture copies the world frame makes are same-size history copies (1920x1080 and 960x540), not the upscale: the 2560x1440 target the last blit shows still gets only the UI during the intro narration (which may be black on purpose).
- `NX_GPU=1` (RTX 2050): device addresses coming back from the title were only translated in RADV's range (>= 0xffff8...); NVIDIA's are low, so completion writes went to wild addresses (SIGSEGV in post_run). `nvn_gpu_addr_in` now tests the span of the handed-out device addresses. NVIDIA then hangs after the first movie dispatch (`dd8b3c1d` is reported "not in the shader library" there, so its pipeline probably fails to build on that driver): not investigated.
