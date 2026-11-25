using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Sm86
{
    // The smaller instructions: carry forms of the adds, byte permutes, warp shuffles and votes, local memory, predicate packing, and the special registers
    static partial class Sm86Emitter
    {
        private static bool IsPredOperand(string s) => s.StartsWith("P") || s.StartsWith("!P") || s.StartsWith("UP") || s == "PT" || s == "UPT";

        // The carry of a + b as a predicate: the sum wrapped below one of its inputs
        private static Operand CarryOf(EmitterContext context, Operand sum, Operand a) => context.ICompareLessUnsigned(sum, a);

        // IADD3 Rd, a, b, RZ with a carry out, or IADD3 Rd, Pd, a, b, RZ: the two-input form (a third input would need two carries)
        private static void Iadd3Carry(EmitterContext context, SassInst inst)
        {
            // Rd, Pd0, Pd1, a, b, c: the carries out of a 34-bit sum, as its bits 32 and 33
            if (inst.Ops.Length == 6 && IsPredOperand(inst.Ops[2]))
            {
                Operand x = Src(context, inst.Ops[3], false), y = Src(context, inst.Ops[4], false), z = Src(context, inst.Ops[5], false);
                Operand s1 = context.IAdd(x, y), s2 = context.IAdd(s1, z);
                Operand c1 = context.ConditionalSelect(CarryOf(context, s1, x), Const(1), Const(0));
                Operand c2 = context.ConditionalSelect(CarryOf(context, s2, s1), Const(1), Const(0));
                Operand total = context.IAdd(c1, c2);

                SetReg(context, inst.Ops[0], s2);
                SetPred(context, inst.Ops[1], context.ICompareNotEqual(context.BitwiseAnd(total, Const(1)), Const(0)));
                SetPred(context, inst.Ops[2], context.ICompareNotEqual(context.ShiftRightU32(total, Const(1)), Const(0)));
                return;
            }

            // Rd, Pd, a, b, c
            if (inst.Ops.Length != 5 || !IsZero(Src(context, inst.Ops[4], false)))
            {
                throw U(inst);
            }

            Operand a = Src(context, inst.Ops[2], false), b = Src(context, inst.Ops[3], false);
            Operand sum = context.IAdd(a, b);
            SetReg(context, inst.Ops[0], sum);
            SetPred(context, inst.Ops[1], CarryOf(context, sum, a));
        }

        // LEA Rd, Pd, Ra, Rb, sh: the 32-bit (Ra << sh) + Rb, its carry in Pd.  LEA.HI[.X] Rd, Ra, Rb, Rc, sh [, Pin]: the high word of ((Rc:Ra) << sh) + Rb, plus the carry in
        private static void LeaFull(EmitterContext context, SassInst inst)
        {
            if (inst.Has("HI") && inst.Has("SX32") && inst.Ops.Length == 4)
            {
                // LEA.HI.SX32 Rd, Ra, Rb, sh: Ra sign-extended to 64 bits, shifted left by sh, its high word, plus Rb: (Ra >> (32 - sh)) arithmetic
                Operand sa = Src(context, inst.Ops[1], false), sb = Src(context, inst.Ops[2], false), ssh = Src(context, inst.Ops[3], false);

                if (ssh.Type != OperandType.Constant)
                {
                    throw U(inst);
                }

                SetReg(context, inst.Ops[0], context.IAdd(ssh.Value == 0 ? context.ShiftRightS32(sa, Const(31)) : context.ShiftRightS32(sa, Const(32 - ssh.Value)), sb));
                return;
            }

            if (inst.Has("HI"))
            {
                Operand a = Src(context, inst.Ops[1], false), b = Src(context, inst.Ops[2], false), c = Src(context, inst.Ops[3], false), sh = Src(context, inst.Ops[4], false);

                if (sh.Type != OperandType.Constant)
                {
                    throw U(inst);
                }

                Operand hi = sh.Value == 0 ? c : context.BitwiseOr(context.ShiftLeft(c, sh), context.ShiftRightU32(a, Const(32 - sh.Value)));
                Operand sum = context.IAdd(hi, b);

                if (inst.Has("X") && inst.Ops.Length > 5)
                {
                    sum = context.IAdd(sum, context.ConditionalSelect(Pred(context, inst.Ops[5]), Const(1), Const(0)));
                }

                SetReg(context, inst.Ops[0], sum);
                return;
            }

            if (inst.Ops.Length == 5 && IsPredOperand(inst.Ops[1]))
            {
                Operand shifted = context.ShiftLeft(Src(context, inst.Ops[2], false), Src(context, inst.Ops[4], false));
                Operand sum = context.IAdd(shifted, Src(context, inst.Ops[3], false));
                SetReg(context, inst.Ops[0], sum);
                SetPred(context, inst.Ops[1], CarryOf(context, sum, shifted));
                return;
            }

            throw U(inst);
        }

        // UIADD3 UR, UP, UR, b, URZ (carry out) is the same operation as IADD3's
        private static void Prmt(EmitterContext context, SassInst inst)
        {
            // PRMT Rd, Ra, sel, Rc: each result byte is one of the eight bytes of Rc:Ra, picked by a selector nibble (bit 3: replicate that byte's sign bit); only a constant selector
            if (inst.Mods.Length != 0 || !TryConst(inst.Ops[2], out int sel))
            {
                throw U(inst);
            }

            Operand a = Src(context, inst.Ops[1], false), c = Src(context, inst.Ops[3], false);
            Operand res = Const(0);

            for (int i = 0; i < 4; i++)
            {
                int nib = (sel >> (4 * i)) & 0xf;
                Operand word = (nib & 4) != 0 ? c : a;
                int shift = (nib & 3) * 8;
                Operand b = (nib & 8) != 0
                    ? context.ConditionalSelect(context.ICompareNotEqual(context.BitwiseAnd(context.ShiftRightU32(word, Const(shift + 7)), Const(1)), Const(0)), Const(0xff), Const(0))
                    : context.BitwiseAnd(context.ShiftRightU32(word, Const(shift)), Const(0xff));
                res = context.BitwiseOr(res, i == 0 ? b : context.ShiftLeft(b, Const(8 * i)));
            }

            SetReg(context, inst.Ops[0], res);
        }

        // SHFL.<IDX|UP|DOWN|BFLY> Pd, Rd, Ra, lane, clamp
        private static void Shfl(EmitterContext context, SassInst inst)
        {
            Operand a = Src(context, inst.Ops[2], false), b = Src(context, inst.Ops[3], false), c = Src(context, inst.Ops[4], false);
            (Operand res, Operand valid) = inst.Has("IDX") ? context.Shuffle(a, b, c)
                : inst.Has("UP") ? context.ShuffleUp(a, b, c)
                : inst.Has("DOWN") ? context.ShuffleDown(a, b, c)
                : inst.Has("BFLY") ? context.ShuffleXor(a, b, c)
                : throw U(inst);

            SetReg(context, inst.Ops[1], res);
            SetPred(context, inst.Ops[0], valid);
        }

        // VOTEU.<ANY|ALL|EQ> UPd, Pa: a uniform predicate from the active lanes' predicate
        private static void Voteu(EmitterContext context, SassInst inst)
        {
            if (inst.Ops.Length == 3)
            {
                Vote(context, inst);       // VOTEU.<ANY|ALL> URd, UPd, Pa: the ballot as well as the vote
                return;
            }

            Operand p = Pred(context, inst.Ops[1]);
            Operand res = inst.Has("ANY") ? context.VoteAny(p) : inst.Has("ALL") ? context.VoteAll(p) : inst.Has("EQ") ? context.VoteAllEqual(p) : throw U(inst);
            SetPred(context, inst.Ops[0], res);
        }

        // LDL.LU / LDS[.U8|.S8|.U16|.S16|.64|.128] Rd, [Ra{.X4} + off] and STL / STS [Ra{.X4} + off], Rs: local and shared memory. .X4 scales the register by four (an element index)
        private static void LocalMem(EmitterContext context, SassInst inst, bool load, StorageKind kind)
        {
            string addr = inst.Ops[load ? 1 : 0];

            if (!addr.StartsWith('['))
            {
                throw U(inst);
            }

            LsSize2 size = LsSize2.B32;
            int words = 1;

            foreach (string m in inst.Mods)
            {
                switch (m)
                {
                    case "LU": break;
                    case "U8": size = LsSize2.U8; break;
                    case "S8": size = LsSize2.S8; break;
                    case "U16": size = LsSize2.U16; break;
                    case "S16": size = LsSize2.S16; break;
                    case "64": size = LsSize2.B64; words = 2; break;
                    case "128": size = LsSize2.B128; words = 4; break;
                    default: throw U(inst);
                }
            }

            string[] parts = addr[1..^1].Split('+');
            Operand baseReg;
            int offset = 0;

            if (parts.Length == 1 && TryConst(parts[0], out int imm))
            {
                baseReg = Const(0);
                offset = imm;
            }
            else
            {
                string reg = parts[0].Trim();
                int scale = 0;

                if (reg.EndsWith(".X4"))
                {
                    reg = reg[..^3];
                    scale = 2;
                }
                else if (reg.EndsWith(".X8"))
                {
                    reg = reg[..^3];
                    scale = 3;
                }
                else if (reg.EndsWith(".X16"))
                {
                    reg = reg[..^4];
                    scale = 4;
                }
                else if (reg.Contains('.'))
                {
                    throw U(inst);
                }

                baseReg = Src(context, reg, false);

                if (scale != 0)
                {
                    baseReg = context.ShiftLeft(baseReg, Const(scale));
                }

                if (parts.Length > 1 && !TryConst(parts[1], out offset))
                {
                    throw U(inst);
                }

                if (parts.Length > 2)
                {
                    throw U(inst);
                }
            }

            int rd = RegIndex(inst.Ops[load ? 0 : 1]);

            if (rd != RZ && rd + words > RZ)
            {
                throw U(inst);
            }

            if (load)
            {
                InstEmit.EmitLoad(context, kind, size, baseReg, rd, offset);
            }
            else
            {
                InstEmit.EmitStore(context, kind, size, baseReg, rd, offset);
            }
        }

        // FSWZADD Rd, Ra, Rb, PNNPPNNP: the quad's four lanes each add or subtract their two operands (derivatives). The pattern is two letters per lane in lane order, the first for Ra and the
        // second for Rb: P adds it, N subtracts it, Z drops it. The shaders pair it with SHFL.BFLY (Ra = the neighbour's value, Rb = its own), and read this way PNNP gives right minus left on
        // the lanes of a pair and PNPNNPNP gives lower minus upper, which is what dFdx and dFdy are
        private static void Fswzadd(EmitterContext context, SassInst inst)
        {
            if (inst.Ops.Length != 4 || inst.Ops[3].Length != 8)
            {
                throw U(inst);
            }

            string pattern = inst.Ops[3];
            Operand lane = context.BitwiseAnd(context.Load(StorageKind.Input, IoVariable.SubgroupLaneId), Const(3));

            Operand Pick(Operand v, int which)
            {
                Operand[] value = new Operand[4];

                for (int l = 0; l < 4; l++)
                {
                    value[l] = pattern[2 * l + which] switch
                    {
                        'P' => v,
                        'N' => context.FPNegate(v),
                        'Z' => ConstF(0),
                        _ => throw U(inst),
                    };
                }

                Operand r = value[3];

                for (int l = 2; l >= 0; l--)
                {
                    r = context.ConditionalSelect(context.ICompareEqual(lane, Const(l)), value[l], r);
                }

                return r;
            }

            Operand a = Src(context, inst.Ops[1], true);
            Operand b = Src(context, inst.Ops[2], true);

            SetFp(context, inst, context.FPAdd(Pick(a, 0), Pick(b, 1)));
        }

        // P2R Rd, PR, Rc, mask: the predicates P0..P6 packed in bits 0..6, under the mask, into Rc
        private static void P2r(EmitterContext context, SassInst inst)
        {
            if (inst.Ops.Length != 4 || inst.Ops[1] != "PR" || !TryConst(inst.Ops[3], out int mask))
            {
                throw U(inst);
            }

            Operand packed = Const(0);

            for (int i = 0; i < 7; i++)
            {
                packed = context.BitwiseOr(packed, context.ShiftLeft(context.ConditionalSelect(Register(i, RegisterType.Predicate), Const(1), Const(0)), Const(i)));
            }

            Operand c = Src(context, inst.Ops[2], false);
            SetReg(context, inst.Ops[0], context.BitwiseOr(context.BitwiseAnd(c, Const(~mask)), context.BitwiseAnd(packed, Const(mask))));
        }

        // S2R Rd, SR_xxx
        private static void S2r(EmitterContext context, SassInst inst)
        {
            Operand v = inst.Ops[1] switch
            {
                "SR_LANEID" => context.Load(StorageKind.Input, IoVariable.SubgroupLaneId),
                "SR_THREAD_KILL" or "SR_VARIABLE_RATE" => Const(0),
                "SR_TID.X" => context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(0)),
                "SR_TID.Y" => context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(1)),
                "SR_TID.Z" => context.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(2)),
                "SR_CTAID.X" => context.Load(StorageKind.Input, IoVariable.CtaId, null, Const(0)),
                "SR_CTAID.Y" => context.Load(StorageKind.Input, IoVariable.CtaId, null, Const(1)),
                "SR_CTAID.Z" => context.Load(StorageKind.Input, IoVariable.CtaId, null, Const(2)),
                "SR_LTMASK" => SubgroupMask(context, IoVariable.SubgroupLtMask),
                "SR_LEMASK" => SubgroupMask(context, IoVariable.SubgroupLeMask),
                "SR_GTMASK" => SubgroupMask(context, IoVariable.SubgroupGtMask),
                "SR_GEMASK" => SubgroupMask(context, IoVariable.SubgroupGeMask),
                "SR_EQMASK" => SubgroupMask(context, IoVariable.SubgroupEqMask),
                _ => throw new Sm86Unsupported("S2R " + inst.Ops[1]),
            };

            SetReg(context, inst.Ops[0], v);
        }

        private static void Flo(EmitterContext context, SassInst inst)
        {
            // FLO[.U32][.SH] Rd, Ra: the index of the highest set bit (all ones when none); .SH gives 31 minus it
            Operand a = Src(context, inst.Ops[1], false);
            Operand msb = inst.Has("S32") ? context.FindMSBS32(a) : context.FindMSBU32(a);
            SetReg(context, inst.Ops[0], inst.Has("SH") ? context.ConditionalSelect(context.ICompareEqual(msb, Const(-1)), Const(-1), context.ISubtract(Const(31), msb)) : msb);
        }
        // The lanes below / at / above this one as a 32-bit mask, whatever the host's subgroup size (the Maxwell warp has 32 lanes)
        private static Operand SubgroupMask(EmitterContext context, IoVariable v)
        {
            int size = context.TranslatorContext.GpuAccessor.QueryHostSubgroupSize();

            if (size <= 32)
            {
                return context.Load(StorageKind.Input, v, null, Const(0));
            }

            Operand lane = context.Load(StorageKind.Input, IoVariable.SubgroupLaneId);
            Operand low = context.Load(StorageKind.Input, v, null, Const(0));
            Operand high = context.Load(StorageKind.Input, v, null, Const(1));

            return context.ConditionalSelect(context.BitwiseAnd(lane, Const(32)), high, low);
        }

        // The 32 lanes of this lane's warp that satisfy the predicate, as a mask
        private static Operand Ballot32(EmitterContext context, Operand p)
        {
            int size = context.TranslatorContext.GpuAccessor.QueryHostSubgroupSize();

            if (size <= 32)
            {
                return context.Ballot(p, 0);
            }

            Operand lane = context.Load(StorageKind.Input, IoVariable.SubgroupLaneId);

            return context.ConditionalSelect(context.BitwiseAnd(lane, Const(32)), context.Ballot(p, 1), context.Ballot(p, 0));
        }

        // VOTE.<ALL|ANY|EQ> Rd, Pd, [!]Pa: Rd the ballot of Pa, Pd the vote
        private static void Vote(EmitterContext context, SassInst inst)
        {
            Operand p = Pred(context, inst.Ops[2]);

            if (inst.Ops[0] != "RZ" && inst.Ops[0] != "URZ")
            {
                SetReg(context, inst.Ops[0], Ballot32(context, p));
            }

            if (inst.Ops[1] != "PT" && inst.Ops[1] != "UPT")
            {
                Operand res = inst.Has("ANY") ? context.VoteAny(p) : inst.Has("ALL") ? context.VoteAll(p) : inst.Has("EQ") ? context.VoteAllEqual(p) : throw U(inst);
                SetPred(context, inst.Ops[1], res);
            }
        }

        // REDUX.<SUM|MIN|MAX|AND|OR|XOR>[.S32] URd, Ra: the reduction of Ra over the active lanes of the warp
        private static void Redux(EmitterContext context, SassInst inst)
        {
            Operand a = Src(context, inst.Ops[1], false);
            bool sgn = inst.Has("S32");
            Instruction op = inst.Has("SUM") ? Instruction.ReduceAdd : inst.Has("MAX") ? (sgn ? Instruction.ReduceMaxS32 : Instruction.ReduceMaxU32) : inst.Has("MIN") ? (sgn ? Instruction.ReduceMinS32 : Instruction.ReduceMinU32) : throw U(inst);

            SetReg(context, inst.Ops[0], context.Add(op, Local(), a));
        }

        // R2P PR, Ra, mask: predicates P0..P6 from the bits of Ra the mask selects
        private static void R2p(EmitterContext context, SassInst inst)
        {
            if (inst.Ops.Length != 3 || inst.Ops[0] != "PR" || !TryConst(inst.Ops[2], out int mask) || inst.Mods.Length != 1 && inst.Mods.Length != 0 && !inst.Has("B1") && !inst.Has("B2") && !inst.Has("B3"))
            {
                throw U(inst);
            }

            int byteSel = inst.Has("B1") ? 8 : inst.Has("B2") ? 16 : inst.Has("B3") ? 24 : 0;
            Operand a = Src(context, inst.Ops[1], false);

            for (int i = 0; i < 7; i++)
            {
                if ((mask >> i & 1) != 0)
                {
                    Operand bit = context.BitwiseAnd(context.ShiftRightU32(a, Const(byteSel + i)), Const(1));
                    context.Copy(Register(i, RegisterType.Predicate), context.ICompareNotEqual(bit, Const(0)));
                }
            }
        }

        // Which atomic a RED / ATOMG names, and whether it is signed
        private static Instruction AtomicOp(SassInst inst, out bool isFloat)
        {
            isFloat = inst.Has("F32") || inst.Has("F16");
            bool sgn = inst.Has("S32") || inst.Has("S64");

            if (inst.Has("ADD")) return Instruction.AtomicAdd;
            if (inst.Has("MIN")) return sgn ? Instruction.AtomicMinS32 : Instruction.AtomicMinU32;
            if (inst.Has("MAX")) return sgn ? Instruction.AtomicMaxS32 : Instruction.AtomicMaxU32;
            if (inst.Has("AND")) return Instruction.AtomicAnd;
            if (inst.Has("OR")) return Instruction.AtomicOr;
            if (inst.Has("XOR")) return Instruction.AtomicXor;
            if (inst.Has("EXCH")) return Instruction.AtomicSwap;

            throw U(inst);
        }

        // RED.E.<op>[.F32.FTZ.RN].STRONG.GPU [address], Rs
        private static void Red(EmitterContext context, SassInst inst)
        {
            if (inst.Ops.Length != 2 || inst.Has("64") || inst.Has("F16") || inst.Has("F64"))
            {
                throw U(inst);
            }

            Instruction op = AtomicOp(inst, out bool isFloat);
            Addr64 a = GlobalAddress(context, inst, inst.Ops[0]);
            Operand v = Src(context, inst.Ops[1], false);

            if (isFloat)
            {
                if (!inst.Has("F32") || !inst.Has("ADD"))
                {
                    throw U(inst);
                }

                context.Add(Instruction.AtomicAddF32, StorageKind.PhysicalMemory, Local(), a.Lo, a.Hi, v);
                return;
            }

            context.Add(op, StorageKind.PhysicalMemory, Local(), a.Lo, a.Hi, v);
        }

        // ATOMG.E.<op>.STRONG.GPU Pd, Rd, [address], Rs: the old value comes back in Rd
        private static void Atomg(EmitterContext context, SassInst inst)
        {
            if (inst.Ops.Length != 4 || inst.Has("64") || inst.Has("F16") || inst.Has("F64") || inst.Has("F32") || inst.Has("CAS"))
            {
                throw U(inst);
            }

            Instruction op = AtomicOp(inst, out _);
            Addr64 a = GlobalAddress(context, inst, inst.Ops[2]);
            Operand v = Src(context, inst.Ops[3], false);
            Operand old = context.Add(op, StorageKind.PhysicalMemory, Local(), a.Lo, a.Hi, v);

            if (inst.Ops[1] != "RZ")
            {
                SetReg(context, inst.Ops[1], old);
            }

            if (inst.Ops[0] != "PT")
            {
                SetPred(context, inst.Ops[0], Const(IrConsts.True));
            }
        }
    }
}
