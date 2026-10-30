# Running a title

```
cmake -S . -B build -G Ninja && cmake --build build          # builds nxlinux, the shader tools and the tests
python3 tools/run_title.py --keys PATH/prod.keys --nsp "PATH/Mario Kart 8 Deluxe.nsp" [--update PATH/update.nsp] --shaders build/shaders.nxsl
```
`run_title.py` extracts the NSP with the vendored `work/nx_extract` (dotnet build of dm/tools/nx_extract from nxnative), unpacks every ExeFS module (`rtld`, `main`, `subsdk0..9`,
`sdk`) into a memory image with `tools/nso.py`, writes `work/run/<title>/modules.txt`, and starts `build/nxlinux --manifest ...`. The keys and dumps stay where you keep them.
`NX_PRESENT=memory` runs without a window; `--entry SYMBOL` starts at a symbol instead of offset 0 of the first module (the synthetic test titles).

Mario Kart 8 Deluxe 4.0.1 (a 64-bit process) boots and shows its title splash: the exact steps, the debugging switches and what is still missing are in
[mk8d-bringup.md](mk8d-bringup.md); `tools/run_mk8d.sh` starts it once the extraction and the shader library exist. An update that comes as `.nsz` is turned into an NSP with
`tools/nsz_to_nsp.py` first. `run_title.py` starts a 32-bit title (the 1.0.0 base) with `--arch32` on its own and takes `--nvn-api` for the version the device reports.

What a real title needs that is not here yet, in the order a boot meets it (the first three exist in a first form; hid, audio and the movies do not):
1. **The keys and the dumps from your own console** (Lockpick_RCM for `prod.keys`, nxdumptool for the game and its updates). Mario Kart 8 Deluxe here is the 1.0.0 base alone; the port targets 4.0.1.
2. **The shader library.** The game's shaders are Maxwell machine code inside its files; `dmsh_xlate` translates them offline (the chain is ported, the script that finds and feeds them
   from an extracted title is not).
3. **Services.** `docs/hle-services.md` lists what is ported and what remains; the file system (`hle_fs.c`) comes first.
4. **Calling guest code from the host** (service events), and the retail loader's entry conventions (the first module's start receives the loader config and the main thread handle).
Expect the first runs to stop on an unimplemented import: each is reported in red with its name, which is the work list.
