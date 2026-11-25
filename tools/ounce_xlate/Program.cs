// Ounce shader -> SPIR-V: the Sm86 frontend of the Ryujinx translator on one program or a directory of them.
//   ounce_xlate FILE.code FILE.sass OUT.spv          one program: the code area (0x80-byte header, instructions) and the text nvdisasm prints for it
//   ounce_xlate --dir DIR [--limit N]                every DIR/<key>.code with a DIR/<key>.sass: writes <key>.spv, and counts the reasons for failures
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using Ryujinx.Graphics.Shader.Translation.Sm86;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

static class OunceXlate
{
    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code, _control; int _binding;     // one descriptor set for all the stages of a pipeline: the vertex stage's bindings start at 1 and the fragment stage's at 64 (as tools/dmsh_xlate numbers them)
        public readonly List<string> Notes = new();
        public Accessor(byte[] code, byte[] control = null) { _code = code; _control = control; _binding = (BitConverter.ToUInt32(code, 0) >> 10 & 15) == 5 ? 64 : 1; }
        public void Log(string message) => Notes.Add(message);
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public int QuerySamplerArrayLengthFromPool() => 32;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
        public int QueryTextureArrayLengthFromPool() => 32;
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1) => SamplerType.Texture2D;
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            return new GpuGraphicsState(false, InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing, false, false, false, false, false, 1f, AlphaTestOp.Always, 0f,
                attr, false, fout, false, false, true, false);
        }
        // A compute program's work group size and shared memory size are in its control section: +0x58, +0x5c, +0x60 (x, y, z) and +0x64 (bytes)
        int Ctl(int off, int def) => _control != null && _control.Length >= off + 4 ? BitConverter.ToInt32(_control, off) : def;
        public int QueryComputeLocalSizeX() => Ctl(0x58, 1);
        public int QueryComputeLocalSizeY() => Ctl(0x5c, 1);
        public int QueryComputeLocalSizeZ() => Ctl(0x60, 1);
        public int QueryComputeSharedMemorySize() => Ctl(0x64, 0);
        public bool QueryHostSupportsTextureShadowLod() => true;
        public bool QueryHostSupportsSeparateSampler() => true;
    }

    // control, c1: the program's control section and constant segment (BRX tables), when they are beside it
    static ShaderProgram Translate(byte[] code, string sass, out Accessor acc, byte[] control = null, byte[] c1 = null)
    {
        acc = new Accessor(code, control);
        return Sm86Translator.Translate(code, sass, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, TranslationFlags.None), control, c1);
    }

    // The reflection tools/shader_pack.py reads (the format tools/dmsh_xlate writes): stage, constant buffers, textures. The stage is the SPH's shader type (bits 10..13 of word 0:
    // 1 vertex, 5 pixel); a program of any other stage is written with "other" and not packed.
    static JsonObject Resources(string key, byte[] code, ShaderProgram p, Accessor acc)
    {
        uint w0 = BitConverter.ToUInt32(code, 0);
        string stage = (w0 & 0x3ff) is 0x81 or 0x82 ? ((w0 >> 10 & 15) switch { 1 => "vs", 5 => "ps", _ => "other" }) : "cs";       // a graphics program starts with an SPH (word 0 & 0x3ff is 0x81 or 0x82); anything else is a compute program, whose code area starts with instructions
        var res = new JsonObject
        {
            ["key"] = key, ["stage"] = stage, ["notes"] = new JsonArray(acc.Notes.Distinct().Select(n => (JsonNode)n).ToArray()),
            ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
            ["geometry_stride"] = 0, ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap,
        };
        foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add(new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot });
        foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add(new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });
        foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
            foreach (var t in list) res[name].AsArray().Add(new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags });
        return res;
    }

    static int Main(string[] args)
    {
        if (args.Length == 3 && args[0] != "--dir")
        {
            ShaderProgram p = Translate(File.ReadAllBytes(args[0]), File.ReadAllText(args[1]), out Accessor acc);
            File.WriteAllBytes(args[2], p.BinaryCode);
            foreach (string n in acc.Notes) Console.Error.WriteLine("note: " + n);
            Console.WriteLine($"{args[2]}: {p.BinaryCode.Length} bytes");
            return 0;
        }
        if (args.Length >= 2 && args[0] == "--dir")
        {
            Sm86Translator.SkipUnsupported = Environment.GetEnvironmentVariable("OUNCE_SKIP") != null;     // OUNCE_SKIP=1: translate what can be, to check the rest with spirv-val
            int limit = args.Length >= 4 && args[2] == "--limit" ? int.Parse(args[3]) : int.MaxValue;
            var reasons = new Dictionary<string, int>(); var notes = new Dictionary<string, int>(); int ok = 0, bad = 0;
            foreach (string codePath in Directory.GetFiles(args[1], "*.code").OrderBy(x => x).Take(limit))
            {
                string sassPath = Path.ChangeExtension(codePath, ".sass");
                if (!File.Exists(sassPath)) continue;
                try
                {
                    string ctlPath = Path.ChangeExtension(codePath, ".ctl"), c1Path = Path.ChangeExtension(codePath, ".c1");
                    ShaderProgram p = Translate(File.ReadAllBytes(codePath), File.ReadAllText(sassPath), out Accessor acc, File.Exists(ctlPath) ? File.ReadAllBytes(ctlPath) : null, File.Exists(c1Path) ? File.ReadAllBytes(c1Path) : null); foreach (string n in acc.Notes.Distinct()) notes[n] = notes.GetValueOrDefault(n) + 1;
                    File.WriteAllBytes(Path.ChangeExtension(codePath, ".spv"), p.BinaryCode);
                    File.WriteAllText(Path.ChangeExtension(codePath, ".res.json"), Resources(Path.GetFileNameWithoutExtension(codePath), File.ReadAllBytes(codePath), p, acc).ToJsonString(new JsonSerializerOptions { WriteIndented = true })); ok++;
                }
                catch (Sm86Unsupported e) { bad++; foreach (string r in e.Message.Split("; ")) reasons[r] = reasons.GetValueOrDefault(r) + 1; }
                catch (Exception e) { bad++; if (Environment.GetEnvironmentVariable("OUNCE_STACK") != null && bad < 4) Console.Error.WriteLine(codePath + "\n" + e); string r = e.GetType().Name + ": " + e.Message.Split('\n')[0]; reasons[r] = reasons.GetValueOrDefault(r) + 1; }
            }
            Console.WriteLine($"translated {ok}, failed {bad}");
            foreach (var kv in reasons.OrderByDescending(k => k.Value).Take(40)) Console.WriteLine($"{kv.Value,6}  {kv.Key}");
            if (notes.Count > 0) { Console.WriteLine("translator notes (programs):"); foreach (var kv in notes.OrderByDescending(k => k.Value).Take(15)) Console.WriteLine($"{kv.Value,6}  {kv.Key}"); }
            return bad == 0 ? 0 : 1;
        }
        Console.Error.WriteLine("usage: ounce_xlate CODE SASS OUT.spv | --dir DIR [--limit N]");
        return 2;
    }
}
