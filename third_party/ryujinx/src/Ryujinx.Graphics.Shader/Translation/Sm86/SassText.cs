using System;
using System.Collections.Generic;
using System.Globalization;

namespace Ryujinx.Graphics.Shader.Translation.Sm86
{
    // One instruction of the text nvdisasm prints for Ampere (SM86) machine code, e.g. `/*0030*/  @!P0 FFMA.SAT R4, R5, -R6, 1 ;` split into its parts. Added for the
    // Switch 2 (Ounce) shader frontend; Maxwell code goes through Decoders/ instead.
    sealed class SassInst
    {
        public int Address;                 // byte address of the instruction
        public int Pred = 7;                // guard predicate, 7 = PT
        public bool PredNot;
        public string Name;                 // the mnemonic without modifiers: FFMA
        public string[] Mods = Array.Empty<string>();   // .SAT .FTZ ...
        public string[] Ops = Array.Empty<string>();    // operands as printed, trimmed
        public string Text;                 // the whole instruction, for messages

        public bool Has(string mod) => Array.IndexOf(Mods, mod) >= 0;
        public override string ToString() => $"{Address:x4}: {Text}";
    }

    static class SassText
    {
        // Instructions in address order, and the label printed before each address (`.L_x_3:` labels the next instruction)
        public static List<SassInst> Parse(string text, int maxInstructions, out Dictionary<int, string> labelAt)
        {
            List<SassInst> insts = new();
            labelAt = new Dictionary<int, string>();
            string pendingLabel = null;

            foreach (string raw in text.Split('\n'))
            {
                string line = raw.Trim();

                if (line.StartsWith(".L_x_") && line.EndsWith(":"))
                {
                    pendingLabel = line[..^1];
                    continue;
                }

                // The address is four hex digits up to 0xfff0 and five from 0x10000 (a program of more than 4096 instructions)
                int close = line.StartsWith("/*") ? line.IndexOf("*/", 2, StringComparison.Ordinal) : -1;

                if (close < 3 || close > 10 || line.Length < close + 3)
                {
                    continue;
                }

                int semi = line.LastIndexOf(';');
                if (semi < 0)
                {
                    continue;
                }

                if (insts.Count >= maxInstructions)
                {
                    break;
                }

                int address = int.Parse(line.AsSpan(2, close - 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                string body = line[(close + 2)..semi].Trim();
                SassInst inst = new() { Address = address, Text = body };

                if (body.StartsWith('@'))
                {
                    int sp = body.IndexOf(' ');
                    string guard = body[1..sp];
                    body = body[(sp + 1)..].Trim();

                    if (guard.StartsWith('!'))
                    {
                        inst.PredNot = true;
                        guard = guard[1..];
                    }

                    // @P0 or @UP0 (a uniform predicate guards the instruction the same way for a single warp-uniform value)
                    inst.Pred = guard == "PT" || guard == "UPT" ? 7 : int.Parse(guard.TrimStart('U').AsSpan(1));
                }

                int space = body.IndexOf(' ');
                string head = space < 0 ? body : body[..space];
                string rest = space < 0 ? "" : body[(space + 1)..].Trim();
                string[] parts = head.Split('.');

                inst.Name = parts[0];
                inst.Mods = parts[1..];
                inst.Ops = SplitOperands(rest);

                if (pendingLabel != null)
                {
                    labelAt[address] = pendingLabel;
                    pendingLabel = null;
                }

                insts.Add(inst);
            }

            return insts;
        }

        // Operands split on the commas outside brackets: `c[0x3][R5+0x10], R4`
        private static string[] SplitOperands(string s)
        {
            List<string> ops = new();
            int depth = 0, start = 0;

            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];

                if (c == '[' || c == '(')
                {
                    depth++;
                }
                else if (c == ']' || c == ')')
                {
                    depth--;
                }
                else if (c == ',' && depth == 0)
                {
                    ops.Add(s[start..i].Trim());
                    start = i + 1;
                }
            }

            if (start < s.Length)
            {
                ops.Add(s[start..].Trim());
            }

            return ops.ToArray();
        }
    }
}
