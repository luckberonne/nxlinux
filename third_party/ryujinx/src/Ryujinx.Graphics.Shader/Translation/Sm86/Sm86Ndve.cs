using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Globalization;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Sm86
{
    // What the title's video-enhancement network needs (Movie/Ndve): packed half-precision arithmetic, the tensor-core multiply HMMA and the matrix loads LDSM. The tensor core works on
    // a whole warp, so both are emulated with warp shuffles: every lane fetches the fragments it needs from the lanes that hold them and does its own dot products in fp32.
    static partial class Sm86Emitter
    {
        // The lane of a 32-thread warp (a subgroup can be wider: the warp is its low or high half)
        private static Operand WarpLane(EmitterContext context) => context.BitwiseAnd(context.Load(StorageKind.Input, IoVariable.SubgroupLaneId), Const(31));

        // The value `v` of lane `want` (0..31) of this lane's warp
        private static Operand FromLane(EmitterContext context, Operand v, Operand want)
        {
            Operand abs = context.BitwiseOr(context.BitwiseAnd(context.Load(StorageKind.Input, IoVariable.SubgroupLaneId), Const(~31)), want);
            (Operand res, _) = context.Shuffle(v, abs, Const(0x1f));
            return res;
        }

        private static Operand FromLane(EmitterContext context, Operand v, int want) => FromLane(context, v, Const(want));


        // The registers a compute launch fills before the first instruction (the programs read R0.., UR4.. without writing them): the spec is "R0=ctax*32+tidx*2;R1=ctay;UR6=ctax;R2=1920"
        // (sums of products of integers and tidx tidy tidz ctax ctay ctaz). It comes from OUNCE_INIT or from the translator's per-program table (Sm86Translator.PreloadSpec).
        public static string PreloadSpec;

        private static void Preload(EmitterContext context)
        {
            string spec = Environment.GetEnvironmentVariable("OUNCE_INIT");

            if (string.IsNullOrEmpty(spec))
            {
                spec = PreloadSpec;
            }

            if (string.IsNullOrEmpty(spec))
            {
                return;
            }

            foreach (string item in spec.Split(';', StringSplitOptions.RemoveEmptyEntries))
            {
                string[] kv = item.Split('=');
                Operand sum = null;

                foreach (string term in kv[1].Split('+'))
                {
                    Operand prod = null;

                    foreach (string atom in term.Split('*'))
                    {
                        Operand a = atom.Trim() switch
                        {
                            "tidx" => context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(0)),
                            "tidy" => context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(1)),
                            "tidz" => context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(2)),
                            "ctax" => context.Load(StorageKind.Input, IoVariable.CtaId, null, Const(0)),
                            "ctay" => context.Load(StorageKind.Input, IoVariable.CtaId, null, Const(1)),
                            "ctaz" => context.Load(StorageKind.Input, IoVariable.CtaId, null, Const(2)),
                            string n => Const(int.Parse(n, CultureInfo.InvariantCulture)),
                        };

                        prod = prod == null ? a : context.IMultiply(prod, a);
                    }

                    sum = sum == null ? prod : context.IAdd(sum, prod);
                }

                SetReg(context, kv[0].Trim(), sum);
            }
        }

        // ---------------------------------------------------------------------------------------------------------------------------------------------------------------- integer odds

        // IABS Rd, Ra
        private static void Iabs(EmitterContext context, SassInst inst)
        {
            Operand a = Src(context, inst.Ops[1], false);
            SetReg(context, inst.Ops[0], context.IAbsNeg(a, true, false));
        }

        // ---------------------------------------------------------------------------------------------------------------------------------------------------------------- packed halves

        // A packed-half source as two floats: R5, R5.H0_H0, R5.H1_H1, R5.H1_H0, -R5, |R5|, a constant buffer word, RZ, or one immediate used for both halves
        private static (Operand, Operand) HalfPair(EmitterContext context, string s)
        {
            s = s.Trim();
            bool neg = false, abs = false;

            if (s.StartsWith('-')) { neg = true; s = s[1..]; }
            if (s.StartsWith('|') && s.EndsWith('|')) { abs = true; s = s[1..^1]; }
            if (s.StartsWith('-')) { neg = !neg; s = s[1..]; }

            Operand lo, hi;

            if (float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out float f) && !s.StartsWith("R"))
            {
                lo = hi = ConstF(f);
            }
            else
            {
                string sel = "H0_H1";
                int dot = s.LastIndexOf('.');

                if (dot > 0 && (s[(dot + 1)..] is "H0_H0" or "H1_H1" or "H0_H1" or "H1_H0"))
                {
                    sel = s[(dot + 1)..];
                    s = s[..dot];
                }

                Operand v = Src(context, s, false);
                Operand l = context.UnpackHalf2x16Low(v), h = context.UnpackHalf2x16High(v);
                lo = sel[1] == '0' ? l : h;
                hi = sel[4] == '0' ? l : h;
            }

            if (abs) { lo = context.FPAbsNeg(lo, true, false); hi = context.FPAbsNeg(hi, true, false); }
            if (neg) { lo = context.FPNegate(lo); hi = context.FPNegate(hi); }

            return (lo, hi);
        }

        private static (Operand, Operand) HalfImmPair(string a, string b)
        {
            float.TryParse(a, NumberStyles.Float, CultureInfo.InvariantCulture, out float x);
            float.TryParse(b, NumberStyles.Float, CultureInfo.InvariantCulture, out float y);
            return (ConstF(x), ConstF(y));
        }

        private static bool IsFloatImm(string s) => float.TryParse(s.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out _) && !s.TrimStart('-').StartsWith("R");

        // HADD2 / HMUL2 / HFMA2 [.F32] [.RELU] [.SAT] Rd, a, b[, c]: two half-precision lanes at once. With .F32 only the low lane is computed and written as a float
        private static void PackedHalf(EmitterContext context, SassInst inst)
        {
            string op = inst.Name;
            (Operand al, Operand ah) = HalfPair(context, inst.Ops[1]);
            Operand bl, bh, cl = null, ch = null;

            if (op == "HFMA2")
            {
                // HFMA2 Rd, a, b, c  or  HFMA2 Rd, a, imm, imm, c (the immediate given once per half)
                if (inst.Ops.Length == 5)
                {
                    (bl, bh) = HalfImmPair(inst.Ops[2], inst.Ops[3]);
                    (cl, ch) = HalfPair(context, inst.Ops[4]);
                }
                else
                {
                    (bl, bh) = HalfPair(context, inst.Ops[2]);
                    (cl, ch) = HalfPair(context, inst.Ops[3]);
                }
            }
            else if (inst.Ops.Length == 4)
            {
                (bl, bh) = HalfImmPair(inst.Ops[2], inst.Ops[3]);
            }
            else
            {
                (bl, bh) = HalfPair(context, inst.Ops[2]);
            }

            Operand rl, rh;

            switch (op)
            {
                case "HADD2": rl = context.FPAdd(al, bl); rh = context.FPAdd(ah, bh); break;
                case "HMUL2": rl = context.FPMultiply(al, bl); rh = context.FPMultiply(ah, bh); break;
                default: rl = context.FPFusedMultiplyAdd(al, bl, cl); rh = context.FPFusedMultiplyAdd(ah, bh, ch); break;
            }

            if (inst.Has("RELU"))
            {
                // max(x, 0) with a NaN going to 0
                rl = context.FPMaximum(rl, ConstF(0f)); rh = context.FPMaximum(rh, ConstF(0f));
            }
            else if (inst.Has("SAT"))
            {
                rl = context.FPSaturate(rl); rh = context.FPSaturate(rh);
            }

            if (inst.Has("F32"))
            {
                SetReg(context, inst.Ops[0], rl);
            }
            else
            {
                SetReg(context, inst.Ops[0], context.PackHalf2x16(rl, rh));
            }
        }

        // F2FP.PACK_AB Rd, Ra, Rb: two floats to two halves, Ra in the high half and Rb in the low one
        private static void F2fp(EmitterContext context, SassInst inst)
        {
            if (!inst.Has("PACK_AB"))
            {
                throw U(inst);
            }

            Operand hi = Src(context, inst.Ops[1], true), lo = Src(context, inst.Ops[2], true);
            SetReg(context, inst.Ops[0], context.PackHalf2x16(lo, hi));
        }

        // ---------------------------------------------------------------------------------------------------------------------------------------------------------------- tensor core

        // The register `delta` places after s, or null for RZ
        private static Operand RegAt(EmitterContext context, string s, int delta)
        {
            int r = RegIndex(s);
            return r == RZ ? Const(0) : Register(r + delta, RegisterType.Gpr);
        }

        // HMMA.16816.F16 Rd, Ra, Rb, Rc: D (16x8, two registers of half pairs) = A (16x16, four registers) * B (16x8, two registers) + C (16x8, two registers).
        // Lane l = 4 g + t holds: A regs a0 = (row g, k 2t..2t+1), a1 = (row g+8, k 2t..), a2 = (row g, k 2t+8..), a3 = (row g+8, k 2t+8..);
        //                          B regs b0 = (k 2t..2t+1, column g), b1 = (k 2t+8.., column g); C and D: reg 0 = (row g, columns 2t..2t+1), reg 1 = (row g+8, columns 2t..2t+1).
        // So the lane needs rows g and g+8 of A from the four lanes 4g..4g+3, and columns 2t and 2t+1 of B from the lanes 8t+t' and 8t+4+t' (t' = 0..3), and does 4 dot products of 16.
        private static void Hmma(EmitterContext context, SassInst inst)
        {
            if (!inst.Has("16816") || !inst.Has("F16"))
            {
                throw U(inst);
            }

            Operand lane = WarpLane(context);
            Operand g = context.ShiftRightU32(lane, Const(2));
            Operand t = context.BitwiseAnd(lane, Const(3));
            Operand rowLane = context.ShiftLeft(g, Const(2));
            Operand colLane0 = context.ShiftLeft(t, Const(3));   // lane of column 2t's holder with t' = 0: (2t) * 4
            Operand colLane1 = context.IAdd(colLane0, Const(4)); // column 2t + 1

            Operand[] areg = new Operand[4], breg = new Operand[2];

            for (int i = 0; i < 4; i++) areg[i] = RegAt(context, inst.Ops[1], i);
            for (int i = 0; i < 2; i++) breg[i] = RegAt(context, inst.Ops[2], i);

            // aVal[row r (0: g, 1: g+8)][k] and bVal[column c (0: 2t, 1: 2t+1)][k]: 16 floats each
            Operand[,] a = new Operand[2, 16], b = new Operand[2, 16];

            for (int tp = 0; tp < 4; tp++)
            {
                Operand srcA = context.IAdd(rowLane, Const(tp));

                for (int i = 0; i < 4; i++)
                {
                    // register i of lane 4g+t': row (i & 1), k pair (i >> 1) * 8 + 2t'
                    Operand v = FromLane(context, areg[i], srcA);
                    a[i & 1, (i >> 1) * 8 + 2 * tp] = context.UnpackHalf2x16Low(v);
                    a[i & 1, (i >> 1) * 8 + 2 * tp + 1] = context.UnpackHalf2x16High(v);
                }

                for (int c = 0; c < 2; c++)
                {
                    Operand srcB = context.IAdd(c == 0 ? colLane0 : colLane1, Const(tp));

                    for (int i = 0; i < 2; i++)
                    {
                        Operand v = FromLane(context, breg[i], srcB);
                        b[c, i * 8 + 2 * tp] = context.UnpackHalf2x16Low(v);
                        b[c, i * 8 + 2 * tp + 1] = context.UnpackHalf2x16High(v);
                    }
                }
            }

            Operand[] cin = new Operand[2];
            for (int i = 0; i < 2; i++) cin[i] = RegAt(context, inst.Ops[3], i);

            Operand[] d = new Operand[4];   // (row 0, col 0), (row 0, col 1), (row 1, col 0), (row 1, col 1)

            for (int r = 0; r < 2; r++)
            {
                Operand cLow = context.UnpackHalf2x16Low(cin[r]), cHigh = context.UnpackHalf2x16High(cin[r]);

                for (int c = 0; c < 2; c++)
                {
                    Operand acc = c == 0 ? cLow : cHigh;

                    for (int k = 0; k < 16; k++)
                    {
                        acc = context.FPFusedMultiplyAdd(a[r, k], b[c, k], acc);
                    }

                    d[r * 2 + c] = acc;
                }
            }

            // write the destination after every source has been read (Rd can be one of the sources)
            Operand r0 = context.PackHalf2x16(d[0], d[1]), r1 = context.PackHalf2x16(d[2], d[3]);
            SetReg(context, inst.Ops[0], r0);
            SetReg(context, RegName(inst.Ops[0], 1), r1);
        }

        // LDSM.16.M88.<1|2|4> Rd.., [Ra + off] and the transposed .MT88: 8x8 matrices of 16-bit values from shared memory. Lane 8i + r supplies the address of row r of matrix i. Without the
        // transpose lane l gets the 32-bit word (row l/4, columns 2(l%4), +1) of each matrix; with it the 16-bit values (row 2(l%4), column l/4) and (row 2(l%4)+1, same column).
        private static void Ldsm(EmitterContext context, SassInst inst)
        {
            bool trans = inst.Has("MT88");

            if (!trans && !inst.Has("M88"))
            {
                throw U(inst);
            }

            int count = inst.Has("4") ? 4 : inst.Has("2") ? 2 : 1;
            string addr = inst.Ops[1];

            if (!addr.StartsWith('['))
            {
                throw U(inst);
            }

            string[] parts = addr[1..^1].Split('+');
            Operand baseAddr = parts[0].Trim() == "RZ" ? Const(0) : Src(context, parts[0].Trim(), false);
            int offset = 0;

            if (parts.Length > 1 && !TryConst(parts[1], out offset))
            {
                throw U(inst);
            }

            Operand mine = context.IAdd(baseAddr, Const(offset));
            Operand lane = WarpLane(context);
            Operand g = context.ShiftRightU32(lane, Const(2));
            Operand t = context.BitwiseAnd(lane, Const(3));
            int smem = context.ResourceManager.SharedMemoryId;

            Operand[] results = new Operand[count];

            for (int i = 0; i < count; i++)
            {
                if (!trans)
                {
                    Operand rowAddr = FromLane(context, mine, context.IAdd(g, Const(8 * i)));
                    Operand byteAddr = context.IAdd(rowAddr, context.ShiftLeft(t, Const(2)));
                    results[i] = context.Load(StorageKind.SharedMemory, smem, context.ShiftRightU32(byteAddr, Const(2)));
                }
                else
                {
                    Operand row0 = context.IAdd(context.ShiftLeft(t, Const(1)), Const(8 * i));
                    Operand rowAddr0 = FromLane(context, mine, row0);
                    Operand rowAddr1 = FromLane(context, mine, context.IAdd(row0, Const(1)));
                    Operand col = context.ShiftLeft(g, Const(1));
                    results[i] = context.BitwiseOr(Half16(context, context.IAdd(rowAddr0, col), smem), context.ShiftLeft(Half16(context, context.IAdd(rowAddr1, col), smem), Const(16)));
                }
            }

            for (int i = 0; i < count; i++)
            {
                SetReg(context, RegName(inst.Ops[0], i), results[i]);
            }
        }

        // The 16-bit value at a byte address of shared memory
        private static Operand Half16(EmitterContext context, Operand byteAddr, int smem)
        {
            Operand word = context.Load(StorageKind.SharedMemory, smem, context.ShiftRightU32(byteAddr, Const(2)));
            Operand shift = context.ShiftLeft(context.BitwiseAnd(byteAddr, Const(2)), Const(3));
            return context.BitwiseAnd(context.ShiftRightU32(word, shift), Const(0xffff));
        }


        // HMNMX2 Rd, Ra, Rb, P: the smaller of each pair of halves when P is true, the larger when it is false (!PT: the larger)
        private static void Hmnmx2(EmitterContext context, SassInst inst)
        {
            (Operand al, Operand ah) = HalfPair(context, inst.Ops[1]);
            (Operand bl, Operand bh) = HalfPair(context, inst.Ops[2]);
            string p = inst.Ops[3].Trim();
            Operand pred = Pred(context, p);
            Operand lo = context.ConditionalSelect(pred, context.FPMinimum(al, bl), context.FPMaximum(al, bl));
            Operand hi = context.ConditionalSelect(pred, context.FPMinimum(ah, bh), context.FPMaximum(ah, bh));
            SetReg(context, inst.Ops[0], context.PackHalf2x16(lo, hi));
        }

        // MOVM.16.MT88 Rd, Rs: transpose an 8x8 matrix of 16-bit values held one pair per lane (lane 4r + c/2 holds row r, columns c, c+1): lane l = 4g + t gets (row 2t, column g) and (row 2t+1, column g)
        private static void Movm(EmitterContext context, SassInst inst)
        {
            if (!inst.Has("MT88"))
            {
                throw U(inst);
            }

            Operand src = Src(context, inst.Ops[1], false);
            Operand lane = WarpLane(context);
            Operand g = context.ShiftRightU32(lane, Const(2));
            Operand t = context.BitwiseAnd(lane, Const(3));
            Operand half = context.BitwiseAnd(g, Const(1));
            Operand shift = context.ShiftLeft(half, Const(4));
            Operand colWord = context.ShiftRightU32(g, Const(1));
            Operand lane0 = context.IAdd(context.ShiftLeft(context.ShiftLeft(t, Const(1)), Const(2)), colWord);
            Operand lane1 = context.IAdd(lane0, Const(4));
            Operand w0 = FromLane(context, src, lane0), w1 = FromLane(context, src, lane1);
            Operand lo = context.BitwiseAnd(context.ShiftRightU32(w0, shift), Const(0xffff));
            Operand hi = context.BitwiseAnd(context.ShiftRightU32(w1, shift), Const(0xffff));
            SetReg(context, inst.Ops[0], context.BitwiseOr(lo, context.ShiftLeft(hi, Const(16))));
        }

        // ---------------------------------------------------------------------------------------------------------------------------------------------------------------- images

        // SUST.P.<2D|3D>.IGN [Rcoords], Rdata, <cbuf>, <word>: a typed store of four floats (R, G, B, A in four consecutive registers) to the image whose handle sits at that word; the
        // coordinates are two or three integers in consecutive registers
        private static void Sust(EmitterContext context, SassInst inst)
        {
            bool is3d = inst.Has("3D");

            if (!inst.Has("P") || !(inst.Has("2D") || is3d) || inst.Ops.Length != 4 || !inst.Ops[0].StartsWith('[') || !TryConst(inst.Ops[2], out int cbufSlot) || !TryConst(inst.Ops[3], out int wordOffset))
            {
                throw U(inst);
            }

            string coords = inst.Ops[0][1..^1];
            int nc = is3d ? 3 : 2;
            Operand[] sources = new Operand[nc + 4];

            for (int i = 0; i < nc; i++)
            {
                sources[i] = Src(context, RegName(coords, i), false);
            }

            for (int i = 0; i < 4; i++)
            {
                sources[nc + i] = Src(context, RegName(inst.Ops[1], i), true);
            }

            SamplerType type = is3d ? SamplerType.Texture3D : SamplerType.Texture2D;
            int packed = TextureHandle.PackOffsets(wordOffset, 0, TextureHandleType.CombinedSampler);
            TextureFlags flags = TextureFlags.None;
            SetBindingPair setAndBinding = context.ResourceManager.GetTextureOrImageBinding(Instruction.ImageStore, type, TextureFormat.Unknown, flags, cbufSlot, packed);
            context.ImageStore(type, TextureFormat.Unknown, flags, setAndBinding, sources);
        }

        // SURED.D.<2D|3D>.<OR|AND|XOR|ADD|MIN|MAX>.STRONG.GPU.IGN [Rcoords], Rvalue, <cbuf>, <word>: an atomic reduction of a 32-bit value into the texel of the image whose handle sits at that
        // word (the title's temporal upscaler ORs flag bits into a mask image). No result comes back
        private static void Sured(EmitterContext context, SassInst inst)
        {
            bool is3d = inst.Has("3D");

            if (!inst.Has("D") || !(inst.Has("2D") || is3d) || inst.Ops.Length != 4 || !inst.Ops[0].StartsWith('[') || !TryConst(inst.Ops[2], out int cbufSlot) || !TryConst(inst.Ops[3], out int wordOffset))
            {
                throw U(inst);
            }

            TextureFlags op = inst.Has("OR") ? TextureFlags.BitwiseOr : inst.Has("AND") ? TextureFlags.BitwiseAnd : inst.Has("XOR") ? TextureFlags.BitwiseXor :
                inst.Has("ADD") ? TextureFlags.Add : inst.Has("MIN") ? TextureFlags.Minimum : inst.Has("MAX") ? TextureFlags.Maximum : throw U(inst);

            string coords = inst.Ops[0][1..^1];
            int nc = is3d ? 3 : 2;
            Operand[] sources = new Operand[nc + 1];

            for (int i = 0; i < nc; i++)
            {
                sources[i] = Src(context, RegName(coords, i), false);
            }

            sources[nc] = Src(context, inst.Ops[1], false);

            SamplerType type = is3d ? SamplerType.Texture3D : SamplerType.Texture2D;
            TextureFormat format = TextureFormat.R32Uint;
            TextureFlags flags = op | TextureFlags.AtomicDiscardResult;
            int packed = TextureHandle.PackOffsets(wordOffset, 0, TextureHandleType.CombinedSampler);
            SetBindingPair setAndBinding = context.ResourceManager.GetTextureOrImageBinding(Instruction.ImageAtomic, type, format, flags, cbufSlot, packed);
            context.ImageAtomic(type, format, flags, setAndBinding, sources);
        }

        // SULD.P.<2D|3D>[.R|.RG|.RGBA].IGN Rd, [Rcoords], <cbuf>, <word>: a typed load of one, two or four floats from the image whose handle sits at that word
        private static void Suld(EmitterContext context, SassInst inst)
        {
            bool is3d = inst.Has("3D");

            if (!inst.Has("P") || !(inst.Has("2D") || is3d) || inst.Ops.Length != 4 || !inst.Ops[1].StartsWith('[') || !TryConst(inst.Ops[2], out int cbufSlot) || !TryConst(inst.Ops[3], out int wordOffset))
            {
                throw U(inst);
            }

            int comps = inst.Has("R") ? 1 : inst.Has("RG") ? 2 : 4;
            string coords = inst.Ops[1][1..^1];
            int nc = is3d ? 3 : 2;
            Operand[] sources = new Operand[nc];

            for (int i = 0; i < nc; i++)
            {
                sources[i] = Src(context, RegName(coords, i), false);
            }

            int rd = RegIndex(inst.Ops[0]);
            Operand[] dests = new Operand[comps];

            for (int i = 0; i < comps; i++)
            {
                dests[i] = rd == RZ ? Local() : Register(rd + i, RegisterType.Gpr);
            }

            SamplerType type = is3d ? SamplerType.Texture3D : SamplerType.Texture2D;
            int packed = TextureHandle.PackOffsets(wordOffset, 0, TextureHandleType.CombinedSampler);
            TextureFlags flags = TextureFlags.None;
            SetBindingPair setAndBinding = context.ResourceManager.GetTextureOrImageBinding(Instruction.ImageLoad, type, TextureFormat.Unknown, flags, cbufSlot, packed);
            context.ImageLoad(type, TextureFormat.Unknown, flags, setAndBinding, (1 << comps) - 1, dests, sources);
        }
    }
}
