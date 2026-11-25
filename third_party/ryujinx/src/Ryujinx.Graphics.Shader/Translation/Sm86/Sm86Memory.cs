using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;

using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Sm86
{
    static partial class Sm86Emitter
    {
        // A 64-bit device address as two words
        private readonly struct Addr64
        {
            public readonly Operand Lo;
            public readonly Operand Hi;

            public Addr64(Operand lo, Operand hi)
            {
                Lo = lo;
                Hi = hi;
            }
        }

        private static Addr64 Add64(EmitterContext context, Addr64 a, Addr64 b)
        {
            Operand lo = context.IAdd(a.Lo, b.Lo);
            Operand carry = context.ConditionalSelect(context.ICompareLessUnsigned(lo, a.Lo), Const(1), Const(0));
            Operand hi = context.IAdd(context.IAdd(a.Hi, b.Hi), carry);

            return new Addr64(lo, hi);
        }

        // The address operand of a global access: [R4.U32+UR6+0x10], [R2], [UR4+0x8c]. A register without .U32 is a 64-bit pair; .U32 zero extends one word.
        private static Addr64 GlobalAddress(EmitterContext context, SassInst inst, string text)
        {
            if (!text.StartsWith('[') || !text.EndsWith(']'))
            {
                throw U(inst);
            }

            Addr64 sum = new(Const(0), Const(0));

            foreach (string part in text[1..^1].Split('+'))
            {
                string tok = part.Trim();
                Addr64 term;

                if (TryConst(tok, out int imm))
                {
                    term = new Addr64(Const(imm), Const(imm < 0 ? -1 : 0));
                }
                else
                {
                    bool word = tok.EndsWith(".U32");
                    string reg = word ? tok[..^4] : tok;

                    if (reg.Contains('.'))
                    {
                        throw U(inst);
                    }

                    if (RegIndex(reg) == RZ)
                    {
                        continue;
                    }

                    Operand lo = Src(context, reg, false);
                    Operand hi = word ? Const(0) : Src(context, RegName(reg, 1), false);

                    term = new Addr64(lo, hi);
                }

                sum = Add64(context, sum, term);
            }

            return sum;
        }

        // LDG.E[.64|.128|.U8|.S8|.U16|.S16][.CONSTANT|.STRONG.SM...] Rd, [address]: raw loads through the device address (buffer device address on Vulkan)
        private static void Ldg(EmitterContext context, SassInst inst)
        {
            if (inst.Ops.Length != 2)
            {
                throw U(inst);
            }

            Addr64 address = GlobalAddress(context, inst, inst.Ops[1]);
            string rd = inst.Ops[0];

            bool small = inst.Has("U8") || inst.Has("S8") || inst.Has("U16") || inst.Has("S16");

            if (small)
            {
                // Load the aligned word and pick the bytes out of it
                bool is8 = inst.Has("U8") || inst.Has("S8");
                bool signed = inst.Has("S8") || inst.Has("S16");
                Operand word = context.Load(StorageKind.PhysicalMemory, context.BitwiseAnd(address.Lo, Const(~3)), address.Hi);
                Operand shift = context.ShiftLeft(context.BitwiseAnd(address.Lo, Const(is8 ? 3 : 2)), Const(3));
                Operand value = signed
                    ? context.BitfieldExtractS32(word, shift, Const(is8 ? 8 : 16))
                    : context.BitfieldExtractU32(word, shift, Const(is8 ? 8 : 16));

                SetReg(context, rd, value);
                return;
            }

            int words = inst.Has("128") ? 4 : inst.Has("64") ? 2 : 1;

            // All words are read before any is written: the address registers may overlap the destination
            Operand[] values = new Operand[words];

            for (int i = 0; i < words; i++)
            {
                Addr64 a = i == 0 ? address : Add64(context, address, new Addr64(Const(4 * i), Const(0)));
                values[i] = context.Load(StorageKind.PhysicalMemory, a.Lo, a.Hi);
            }

            for (int i = 0; i < words; i++)
            {
                SetReg(context, RegName(rd, i), values[i]);
            }
        }

        // STG.E[.64|.128] [address], Rs
        private static void Stg(EmitterContext context, SassInst inst)
        {
            if (inst.Ops.Length != 2 || inst.Has("U8") || inst.Has("S8") || inst.Has("U16") || inst.Has("S16"))
            {
                throw U(inst);
            }

            Addr64 address = GlobalAddress(context, inst, inst.Ops[0]);
            string rs = inst.Ops[1];
            int words = inst.Has("128") ? 4 : inst.Has("64") ? 2 : 1;

            for (int i = 0; i < words; i++)
            {
                Addr64 a = i == 0 ? address : Add64(context, address, new Addr64(Const(4 * i), Const(0)));
                context.Store(StorageKind.PhysicalMemory, a.Lo, a.Hi, Src(context, RegName(rs, i), false));
            }
        }
    }
}
