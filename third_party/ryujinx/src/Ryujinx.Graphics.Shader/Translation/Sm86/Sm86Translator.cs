using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using System.Globalization;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Sm86
{
    public sealed class Sm86Unsupported : Exception
    {
        public Sm86Unsupported(string what) : base(what) { }
    }

    // The frontend for Ampere (SM86) machine code, the GPU of Switch 2 (the "Ounce" shader archives of its titles). It takes the program as the 0x80-byte header followed by the
    // instructions, and their text as nvdisasm prints it, and builds the same intermediate representation the Maxwell instructions of Instructions/ do, so everything from SSA to
    // the SPIR-V generator is the one translator.
    public static class Sm86Translator
    {
        public const int HeaderSize = 0x80;

        // Skip the unsupported instructions instead of failing (to see whether everything else in a program translates to valid SPIR-V)
        public static bool SkipUnsupported { get => Sm86Emitter.SkipUnsupported; set => Sm86Emitter.SkipUnsupported = value; }

        // code: the program (header, then instructions); sass: nvdisasm's text for the instructions
        // The jump tables of BRX: the control section lists them from +0xe0 (a count, then {address of the BRX, entries, flags, offset in c[1]} each) and c[1] holds the entries,
        // which are instruction addresses
        public static Dictionary<int, int[]> JumpTables(byte[] control, byte[] c1)
        {
            Dictionary<int, int[]> tables = new();

            if (control == null || c1 == null || control.Length < 0xe4)
            {
                return tables;
            }

            uint count = BitConverter.ToUInt32(control, 0xe0);

            for (int i = 0; i < count && 0xe4 + 16 * (i + 1) <= control.Length; i++)
            {
                int at = 0xe4 + 16 * i;
                int address = BitConverter.ToInt32(control, at), n = BitConverter.ToInt32(control, at + 4), offset = BitConverter.ToInt32(control, at + 12);

                if (n <= 0 || n > 0x10000 || offset < 0 || (long)offset + 4L * n > c1.Length)
                {
                    continue;
                }

                int[] entries = new int[n];

                for (int k = 0; k < n; k++)
                {
                    entries[k] = BitConverter.ToInt32(c1, offset + 4 * k);
                }

                tables[address] = entries;
            }

            return tables;
        }

        // Registers a program may be given at its start (OUNCE_INIT overrides): none by default. A compute program has no header, so its prologue (S2R of the thread and group ids) is in the code; the registers once thought to be filled by the launch were that prologue, dropped with a 0x50-byte "header". The first program of
        // the title's video enhancement reads the work group's x and y ids and the thread's x in R0, R1 and R5 and the picture's size in R2 and R3
        private static readonly Dictionary<string, string> PreloadTable = new()
        {
        };

        public static ShaderProgram Translate(byte[] code, string sass, IGpuAccessor gpuAccessor, TranslationOptions options, byte[] control = null, byte[] c1 = null)
        {
            int ninst = (code.Length - ((BitConverter.ToUInt32(code, 0) & 0x3ff) is 0x81 or 0x82 ? HeaderSize : 0)) / 16;
            List<SassInst> insts = SassText.Parse(sass, ninst, out Dictionary<int, string> labelAt);

            // Targets of BRX have no label in the listing (nothing branches to them by name): give each one
            Dictionary<int, int[]> jumpTables = JumpTables(control, c1);

            // RET.REL.NODEC Rk returns to the address a `MOV Rk, <address after the call>` put there before a CALL.REL.NOINC: the returns of one register are its table
            for (int ri = 0; ri < insts.Count; ri++)
            {
                SassInst ret = insts[ri];

                if (ret.Name != "RET" || !ret.Has("REL") || ret.Ops.Length < 1 || jumpTables.ContainsKey(ret.Address))
                {
                    continue;
                }

                // The calls proper put their return address in a register by a MOV just before (the other CALL.REL.NOINCs are jumps to reconvergence points). The function a RET ends is the nearest
                // call target at or before it, and it returns to where any of the calls to it came from (the address may have moved to another register on the way, so the register is not matched)
                List<(int Target, int Back)> calls = new();

                for (int ci = 1; ci < insts.Count; ci++)
                {
                    SassInst call = insts[ci], mov = insts[ci - 1];

                    if (call.Name == "CALL" && call.Has("NOINC") && call.Ops.Length == 1 && mov.Name == "MOV" && mov.Ops.Length == 2 && int.TryParse(mov.Ops[1].StartsWith("0x") ? mov.Ops[1][2..] : "x", System.Globalization.NumberStyles.HexNumber, null, out int back) && back == call.Address + 0x10)
                    {
                        string lab = call.Ops[0][2..^1];

                        foreach (KeyValuePair<int, string> kv in labelAt)
                        {
                            if (kv.Value == lab)
                            {
                                calls.Add((kv.Key, back));
                            }
                        }
                    }
                }

                int fn = -1;

                foreach ((int target, _) in calls)
                {
                    if (target <= ret.Address && target > fn)
                    {
                        fn = target;
                    }
                }

                List<int> returns = new();

                foreach ((int target, int back) in calls)
                {
                    if (target == fn)
                    {
                        returns.Add(back);
                    }
                }

                if (returns.Count != 0)
                {
                    jumpTables[ret.Address] = returns.ToArray();
                }
            }

            foreach (int[] entries in jumpTables.Values)
            {
                foreach (int t in entries)
                {
                    labelAt.TryAdd(t, $".L_x_brx{t:x}");
                }
            }

            // OUNCE_CUT=<hex address>: the program ends there (debugging: the part before it can be run on its own)
            if (Environment.GetEnvironmentVariable("OUNCE_CUT") is string cut && int.TryParse(cut, System.Globalization.NumberStyles.HexNumber, null, out int cutAt))
            {
                int keep = insts.FindIndex(i => i.Address >= cutAt);

                if (keep >= 0)
                {
                    insts.RemoveRange(keep, insts.Count - keep);
                    if (Environment.GetEnvironmentVariable("OUNCE_CUTREG") is string reg)
                    {
                        // OUNCE_CUTREG=<n>: before the exit, store R<n> to c[0][0x10] + lane * 4, so the code that computes it is not dead
                        insts.Add(new SassInst { Address = cutAt, Name = "ULDC", Mods = new[] { "64" }, Ops = new[] { "UR60", "c[0x0][0x10]" }, Text = "ULDC.64 UR60, c[0x0][0x10]" });
                        insts.Add(new SassInst { Address = cutAt, Name = "S2R", Ops = new[] { "R250", "SR_TID.X" }, Text = "S2R R250, SR_TID.X" });
                        insts.Add(new SassInst { Address = cutAt, Name = "IMAD", Mods = new[] { "SHL", "U32" }, Ops = new[] { "R250", "R250", "0x4", "RZ" }, Text = "IMAD.SHL.U32 R250, R250, 0x4, RZ" });
                        insts.Add(new SassInst { Address = cutAt, Name = "STG", Mods = new[] { "E" }, Ops = new[] { "[R250.U32+UR60]", "R" + reg }, Text = "STG.E [R250.U32+UR60], R" + reg });
                    }

                    insts.Add(new SassInst { Address = cutAt, Name = "EXIT", Ops = Array.Empty<string>(), Text = "EXIT" });
                }
            }

            // OUNCE_PASSTHRU=1 (vertex programs): an attribute the program reads (ALD) and never writes (AST) goes out unchanged, as if the hardware's attribute memory held the input where the output goes
            if (Environment.GetEnvironmentVariable("OUNCE_PASSTHRU") != null && (BitConverter.ToUInt32(code, 0) >> 10 & 15) == 1)
            {
                SortedDictionary<int, int> reads = new();
                HashSet<int> writes = new();

                foreach (SassInst i in insts)
                {
                    if ((i.Name == "ALD" || i.Name == "AST") && i.Ops.Length >= 2)
                    {
                        string a = i.Name == "ALD" ? i.Ops[1] : i.Ops[0];
                        int w = i.Has("128") ? 4 : i.Has("64") ? 2 : 1;

                        if (a.StartsWith("a[0x") && int.TryParse(a[4..a.IndexOf(']')], System.Globalization.NumberStyles.HexNumber, null, out int off) && off >= 0x80 && off < 0x280)
                        {
                            if (i.Name == "ALD" && !a.Contains('+')) reads[off] = Math.Max(reads.GetValueOrDefault(off), w);
                            else if (i.Name == "AST") for (int k = 0; k < w; k++) writes.Add(off + 4 * k);
                        }
                    }
                }

                for (int e = insts.Count - 1; e >= 0; e--)
                {
                    if (insts[e].Name != "EXIT")
                    {
                        continue;
                    }

                    List<SassInst> add = new();

                    foreach ((int off, int w) in reads)
                    {
                        if (writes.Contains(off))
                        {
                            continue;
                        }

                        string wm = w == 4 ? "128" : w == 2 ? "64" : "32";
                        add.Add(new SassInst { Address = insts[e].Address, Pred = insts[e].Pred, PredNot = insts[e].PredNot, Name = "ALD", Mods = new[] { wm }, Ops = new[] { "R250", $"a[0x{off:x}]" }, Text = $"ALD.{wm} R250, a[0x{off:x}]" });
                        add.Add(new SassInst { Address = insts[e].Address, Pred = insts[e].Pred, PredNot = insts[e].PredNot, Name = "AST", Mods = new[] { wm }, Ops = new[] { $"a[0x{off:x}]", "R250" }, Text = $"AST.{wm} a[0x{off:x}], R250" });
                    }

                    insts.InsertRange(e, add);
                }
            }

            // The program ends where the compiler's own trailing `BRA self` is: nothing after it runs
            // ... unless something before it branches or calls past it: the subroutines of a large program sit after that spin
            Dictionary<string, int> addressOfLabel = new();

            foreach (KeyValuePair<int, string> kv in labelAt)
            {
                addressOfLabel[kv.Value] = kv.Key;
            }

            int farthest = -1;

            for (int i = 0; i < insts.Count; i++)
            {
                if (insts[i].Name == "BRA" && labelAt.TryGetValue(insts[i].Address, out string l) && insts[i].Ops.Length == 1 && insts[i].Ops[0] == $"`({l})" && farthest <= insts[i].Address)
                {
                    insts.RemoveRange(i, insts.Count - i);
                    break;
                }

                if (insts[i].Name is "BRA" or "CALL" or "BSSY")
                {
                    foreach (string op in insts[i].Ops)
                    {
                        int tick = op.IndexOf("`(", StringComparison.Ordinal);

                        if (tick >= 0 && addressOfLabel.TryGetValue(op[(tick + 2)..op.IndexOf(')', tick)], out int target) && target > farthest)
                        {
                            farthest = target;
                        }
                    }
                }
            }

            if (Environment.GetEnvironmentVariable("OUNCE_DEBUG_LABELS") != null)
            {
                Console.Error.WriteLine($"insts {insts.Count}, last {(insts.Count > 0 ? insts[^1].Address : -1):x}, labels {labelAt.Count}, has 0x14530: {labelAt.ContainsKey(0x14530)}, ninst {(code.Length) / 16}");
            }

            // The Maxwell header parser reads the first 0x50 bytes: SPH version 4 keeps their layout (stage, kill and store flags, the pixel output map)
            // SPH word 0 bits 10..13 is the shader type: 1 vertex, 5 pixel, 14 compute (a compute program has no graphics header to speak of: its work group size and shared memory come from the accessor,
            // which reads them from the control section)
            ShaderDefinitions definitions;
            int localMemorySize;

            if ((BitConverter.ToUInt32(code, 0) & 0x3ff) is not (0x81 or 0x82))      // a graphics program starts with an SPH, a compute program with instructions
            {
                definitions = new ShaderDefinitions(ShaderStage.Compute, gpuAccessor.QueryComputeLocalSizeX(), gpuAccessor.QueryComputeLocalSizeY(), gpuAccessor.QueryComputeLocalSizeZ());
                localMemorySize = gpuAccessor.QueryComputeLocalMemorySize();
                options = new TranslationOptions(options.TargetLanguage, options.TargetApi, options.Flags | TranslationFlags.Compute);
            }
            else
            {
                ShaderHeader header = new(gpuAccessor, 0);
                definitions = Translator.CreateGraphicsDefinitions(gpuAccessor, header);
                localMemorySize = Translator.GetLocalMemorySize(header);
            }

            AttributeUsage attributeUsage = new(gpuAccessor);
            FeatureFlags features = FeatureFlags.None;
            byte clip = 0;

            Sm86Emitter.PreloadSpec = control != null && control.Length >= 0x28 ? PreloadTable.GetValueOrDefault(Convert.ToHexString(control, 0x20, 8).ToLowerInvariant()) : null;

            Sm86Emitter.Prescan(insts, definitions, attributeUsage, ref features, ref clip);

            DecodedProgram program = new(null, new Dictionary<ulong, DecodedFunction>(), attributeUsage, features, clip, 0);
            TranslatorContext context = new(0, code.Length, localMemorySize, definitions, gpuAccessor, options, program);

            return context.TranslateCustom(ctx => Sm86Emitter.Emit(ctx, insts, labelAt, jumpTables), features, clip);
        }
    }
}
