using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using System.Globalization;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Sm86
{
    // Instruction semantics of Ampere machine code. Each mnemonic the translator knows is a case of Emit1; anything else throws Sm86Unsupported with the instruction's text, so a census
    // of the library says what is missing, in the order that matters.
    //
    // Registers: R0..R254 are the GPR bank of the intermediate representation, RZ reads 0. The uniform registers UR0..UR62 live in the same bank above RZ (index 256 + n) and the
    // uniform predicates UP0..UP6 in the predicate bank above PT (index 8 + n); URZ and UPT are the constants. (RegisterConsts.SsaGprs/SsaPreds size the SSA tables for them.)
    static partial class Sm86Emitter
    {
        // For checking the rest of a program's translation: an instruction that is not supported is skipped (its destination keeps whatever it had) instead of failing the program
        public static bool SkipUnsupported;
        public static string[] Disabled = Environment.GetEnvironmentVariable("OUNCE_DISABLE")?.Split(',');       // debugging: mnemonics to treat as unsupported

        private const int RZ = 255;
        private const int UrBase = 256;
        private const int UpBase = 8;

        private sealed class State
        {
            public EmitterContext Context;
            public Func<string, Operand> LabelOf;
            public readonly Dictionary<int, string> BarrierTarget = new();   // B0..B15: the label the latest BSSY on it named
            public Dictionary<int, int[]> JumpTables;       // BRX address -> the instruction addresses its table holds
            public Dictionary<int, string> LabelAt;         // the label printed before each address, and one made for each BRX target
        }

        // What the Maxwell decoder learns by scanning (Decoders/Decoder.cs): which attributes the program reads and writes, and the features it uses
        public static void Prescan(List<SassInst> insts, ShaderDefinitions definitions, AttributeUsage usage, ref FeatureFlags features, ref byte clip)
        {
            foreach (SassInst inst in insts)
            {
                switch (inst.Name)
                {
                    case "SHFL":
                    case "HMMA":
                        features |= FeatureFlags.Shuffle;
                        break;
                    case "MOVM":
                        features |= FeatureFlags.Shuffle;
                        break;
                    case "LDSM":
                        features |= FeatureFlags.Shuffle | FeatureFlags.SharedMemory;
                        break;
                    case "TEX":
                    case "TLD":
                    case "TLD4":
                    case "TXD":
                    case "TMML":
                    case "TXQ":
                        if (inst.Has("B") && !(inst.Name == "TLD4" && !inst.Has("R") && !inst.Has("G") && !inst.Has("A")))
                        {
                            features |= FeatureFlags.Bindless;
                        }

                        break;
                    case "LDL":
                    case "STL":
                        features |= FeatureFlags.LocalMemory;
                        break;
                    case "ATOMS":
                    case "LDS":
                    case "STS":
                        features |= FeatureFlags.SharedMemory;
                        break;
                    case "ATOM":
                    case "ATOMG":
                    case "RED":
                    case "STG":
                    case "SUST":
                    case "SURED":
                    case "SUATOM":
                        features |= FeatureFlags.Store;
                        break;
                }

                bool isStore = inst.Name == "AST";
                bool isLoad = inst.Name == "ALD" || inst.Name == "IPA";

                if (!isStore && !isLoad)
                {
                    continue;
                }

                string attr = Array.Find(inst.Ops, o => o.StartsWith("a["));

                if (attr == null || !TryConst(attr[2..^1], out int offset))
                {
                    throw new Sm86Unsupported("indirect attribute access");
                }

                int count = AccessWords(inst);

                for (int i = 0; i < count; i++)
                {
                    int a = offset + i * 4;

                    if (a >= AttributeConsts.UserAttributeBase && a < AttributeConsts.UserAttributeEnd)
                    {
                        int userAttr = a - AttributeConsts.UserAttributeBase;

                        if (isStore)
                        {
                            usage.SetOutputUserAttribute(userAttr / 16);
                        }
                        else
                        {
                            usage.SetInputUserAttribute(userAttr / 16, (userAttr >> 2) & 3);
                        }
                    }
                    else if (!isStore && (a == AttributeConsts.FogCoord || (a >= AttributeConsts.FrontColorDiffuseR && a < AttributeConsts.ClipDistance0) ||
                        (a >= AttributeConsts.TexCoordBase && a < AttributeConsts.TexCoordEnd)))
                    {
                        features |= FeatureFlags.FixedFuncAttr;
                    }
                    else if (isStore)
                    {
                        if (a == AttributeConsts.Layer && definitions.Stage is not ShaderStage.Compute and not ShaderStage.Fragment)
                        {
                            features |= FeatureFlags.RtLayer;
                        }
                        else if (a == AttributeConsts.ViewportIndex && definitions.Stage != ShaderStage.Fragment)
                        {
                            features |= FeatureFlags.ViewportIndex;
                        }
                        else if (a >= AttributeConsts.ClipDistance0 && a <= AttributeConsts.ClipDistance7 && definitions.Stage.IsVtg)
                        {
                            clip |= (byte)(1 << ((a - AttributeConsts.ClipDistance0) / 4));
                        }
                        else if (a == AttributeConsts.ViewportMask && definitions.Stage != ShaderStage.Fragment)
                        {
                            features |= FeatureFlags.ViewportMask;
                        }
                    }
                    else if ((a == AttributeConsts.PositionX || a == AttributeConsts.PositionY) && definitions.Stage == ShaderStage.Fragment)
                    {
                        features |= FeatureFlags.FragCoordXY;
                    }
                    else if (a == AttributeConsts.InstanceId && definitions.Stage == ShaderStage.Vertex)
                    {
                        features |= FeatureFlags.InstanceId;
                    }
                }
            }
        }

        // The words an AST/ALD moves: .64 two, .96 three, .128 four, none one
        private static int AccessWords(SassInst inst)
        {
            for (int i = inst.Mods.Length - 1; i >= 0; i--)
            {
                if (int.TryParse(inst.Mods[i], out int bits) && bits >= 32)
                {
                    return bits / 32;
                }
            }

            return 1;
        }

        public static void Emit(EmitterContext context, List<SassInst> insts, Dictionary<int, string> labelAt, Dictionary<int, int[]> jumpTables = null)
        {
            Dictionary<string, Operand> labels = new();

            Operand LabelOf(string name)
            {
                if (!labels.TryGetValue(name, out Operand l))
                {
                    labels[name] = l = Label();
                }

                return l;
            }

            State st = new() { Context = context, LabelOf = LabelOf, JumpTables = jumpTables ?? new(), LabelAt = labelAt };

            Preload(context);
            SortedSet<string> unsupported = new();

            bool dead = false;      // after an unconditional KILL, EXIT or branch nothing runs until the next label (the generator wants a discard or a return to end its block)

            foreach (SassInst inst in insts)
            {
                if (labelAt.TryGetValue(inst.Address, out string name))
                {
                    context.MarkLabel(LabelOf(name));
                    dead = false;
                }

                if (dead)
                {
                    continue;
                }

                Operand skip = null;
                bool selfGuarded = inst.Name == "BRA" || inst.Name == "EXIT" || inst.Name == "BSYNC" || inst.Name == "BREAK" || inst.Name == "CALL" || inst.Name is "BRX" or "BRXU" || inst.Name == "RET";

                if (inst.Pred != 7 && !selfGuarded)
                {
                    skip = Label();
                    Operand p = Register(inst.Pred, RegisterType.Predicate);

                    if (inst.PredNot)
                    {
                        context.BranchIfTrue(skip, p);
                    }
                    else
                    {
                        context.BranchIfFalse(skip, p);
                    }
                }

                try
                {
                    Emit1(st, inst);
                }
                catch (Sm86Unsupported e)
                {
                    // The program cannot be translated, but the rest of it is still read: the census wants every mnemonic that is missing, not the first
                    unsupported.Add(e.Message);
                }

                if (skip != null)
                {
                    context.MarkLabel(skip);
                }
                else if (inst.Pred == 7 && inst.Name is "KILL" or "EXIT" or "BRA" or "BSYNC" or "CALL" or "BRX" or "BRXU" or "RET" && inst.Ops.Length <= 1)
                {
                    dead = true;
                }
            }

            if (Environment.GetEnvironmentVariable("OUNCE_DEBUG_LABELS") != null)
            {
                // A branch to a label the listing never defines leaves the structurer with a goto that has no place to go
                HashSet<string> defined = new(labelAt.Values);

                foreach (string used in labels.Keys)
                {
                    if (!defined.Contains(used))
                    {
                        Console.Error.WriteLine("label used but not defined: " + used);
                    }
                }
            }

            if (unsupported.Count != 0 && !SkipUnsupported)
            {
                throw new Sm86Unsupported(string.Join("; ", unsupported));
            }
        }

        // `@P0 EXIT` / `@!P0 BRA L`: the branch is taken when the guard holds
        private static Operand Guard(EmitterContext context, SassInst inst)
        {
            if (inst.Pred == 7)
            {
                return null;
            }

            Operand p = Register(inst.Pred, RegisterType.Predicate);

            return inst.PredNot ? context.BitwiseNot(p) : p;
        }

        private static void Emit1(State st, SassInst inst)
        {
            EmitterContext context = st.Context;
            string name = inst.Name;

            if (Disabled != null && Array.IndexOf(Disabled, name) >= 0)
            {
                throw U(inst);
            }

            // The uniform datapath instructions are the same operations on the uniform registers
            if (name.Length > 1 && name[0] == 'U' && name is "UMOV" or "UIADD3" or "USHF" or "ULOP3" or "UISETP" or "USEL" or "UIMAD" or "ULEA" or "ULDC" or "UPLOP3" or "UFLO" or "UPOPC" or "UBREV" or "UPRMT")
            {
                name = name[1..];
            }

            switch (name)
            {
                case "NOP":
                case "DEPBAR":
                case "WARPSYNC":
                case "BAR":
                    break;

                case "MOV":
                case "R2UR":
                    SetReg(context, inst.Ops[0], Src(context, inst.Ops[1], false));
                    break;

                case "CS2R":
                {
                    // CS2R Rd, SRZ: the 64-bit register pair Rd:Rd+1 is cleared (other special registers are clocks)
                    if (inst.Ops[1] != "SRZ")
                    {
                        throw U(inst);
                    }

                    int rd = RegIndex(inst.Ops[0]);
                    SetReg(context, inst.Ops[0], Const(0));
                    if (rd != RZ && !inst.Has("32"))
                    {
                        SetReg(context, RegName(inst.Ops[0], 1), Const(0));
                    }

                    break;
                }

                case "IMAD":
                    Imad(context, inst);
                    break;

                case "IADD3":
                    Iadd3(context, inst);
                    break;

                case "LEA":
                    Lea(context, inst);
                    break;

                case "PRMT": Prmt(context, inst); break;
                case "SHFL": Shfl(context, inst); break;
                case "VOTEU": Voteu(context, inst); break;
                case "FSWZADD": Fswzadd(context, inst); break;
                case "LDL": LocalMem(context, inst, true, StorageKind.LocalMemory); break;
                case "STL": LocalMem(context, inst, false, StorageKind.LocalMemory); break;
                case "LDS": LocalMem(context, inst, true, StorageKind.SharedMemory); break;
                case "STS": LocalMem(context, inst, false, StorageKind.SharedMemory); break;
                case "LDG": Ldg(context, inst); break;
                case "STG": Stg(context, inst); break;
                case "TEX": Tex(context, inst, false); break;
                case "TLD": Tex(context, inst, false, true); break;
                case "TLD4": Tex(context, inst, true); break;
                case "TXD": Txd(context, inst); break;
                case "TXQ": Txq(context, inst); break;
                case "TMML": Tmml(context, inst); break;
                case "P2R": P2r(context, inst); break;
                case "S2R": S2r(context, inst); break;
                case "S2UR": S2r(context, inst); break;
                case "FLO": Flo(context, inst); break;
                case "BREV": SetReg(context, inst.Ops[0], context.BitfieldReverse(Src(context, inst.Ops[1], false))); break;
                case "VOTE": Vote(context, inst); break;
                case "REDUX": Redux(context, inst); break;
                case "R2P": R2p(context, inst); break;
                case "RED": Red(context, inst); break;
                case "ATOMG": Atomg(context, inst); break;
                case "POPC": SetReg(context, inst.Ops[0], context.BitCount(Src(context, inst.Ops[1], false))); break;
                case "BREAK": break;       // a thread leaves its convergence barrier; the branch that follows does the leaving

                case "LOP3":
                    Lop3(context, inst);
                    break;

                case "SHF":
                    Shf(context, inst);
                    break;

                case "SGXT":
                {
                    // SGXT[.U32] Rd, Ra, bits: sign-extend (or, with .U32, zero-extend) the low `bits` bits of Ra
                    Operand a = Src(context, inst.Ops[1], false), n = Src(context, inst.Ops[2], false);
                    Operand res = inst.Has("U32") ? context.BitfieldExtractU32(a, Const(0), n) : context.BitfieldExtractS32(a, Const(0), n);
                    SetReg(context, inst.Ops[0], res);
                    break;
                }

                case "IMNMX":
                {
                    Operand a = Src(context, inst.Ops[1], false), b = Src(context, inst.Ops[2], false), p = Pred(context, inst.Ops[3]);
                    bool u = inst.Has("U32");
                    Operand lo = u ? context.IMinimumU32(a, b) : context.IMinimumS32(a, b), hi = u ? context.IMaximumU32(a, b) : context.IMaximumS32(a, b);
                    SetReg(context, inst.Ops[0], context.ConditionalSelect(p, lo, hi));
                    break;
                }

                case "SEL":
                case "FSEL":
                    SetReg(context, inst.Ops[0], context.ConditionalSelect(Pred(context, inst.Ops[3]), Src(context, inst.Ops[1], name == "FSEL"), Src(context, inst.Ops[2], name == "FSEL")));
                    break;

                case "ISETP":
                    Isetp(context, inst);
                    break;

                case "FSETP":
                    Fsetp(context, inst);
                    break;

                case "FSET":
                    Fset(context, inst);
                    break;

                case "PLOP3":
                    Plop3(context, inst);
                    break;

                case "FADD":
                    SetFp(context, inst, context.FPAdd(Src(context, inst.Ops[1], true), Src(context, inst.Ops[2], true)));
                    break;

                case "FMUL":
                    SetFp(context, inst, context.FPMultiply(Src(context, inst.Ops[1], true), Src(context, inst.Ops[2], true)));
                    break;

                case "FFMA":
                    SetFp(context, inst, context.FPFusedMultiplyAdd(Src(context, inst.Ops[1], true), Src(context, inst.Ops[2], true), Src(context, inst.Ops[3], true)));
                    break;

                case "FMNMX":
                {
                    // FMNMX Rd, Ra, Rb, P: the smaller when P holds, the larger otherwise; a NaN input yields the other one
                    Operand a = Src(context, inst.Ops[1], true), b = Src(context, inst.Ops[2], true), p = Pred(context, inst.Ops[3]);
                    SetFp(context, inst, context.ConditionalSelect(p, context.FPMinimum(a, b), context.FPMaximum(a, b)));
                    break;
                }

                case "MUFU":
                    Mufu(context, inst);
                    break;

                case "FRND":
                {
                    Operand a = Src(context, inst.Ops[1], true);
                    Operand res = inst.Has("FLOOR") ? context.FPFloor(a) : inst.Has("CEIL") ? context.FPCeiling(a) : inst.Has("TRUNC") ? context.FPTruncate(a) : context.FPRound(a);
                    SetReg(context, inst.Ops[0], res);
                    break;
                }

                case "F2I":
                    F2i(context, inst);
                    break;

                case "I2FP":
                case "I2F":
                    I2f(context, inst);
                    break;

                case "HADD2":
                case "HMUL2":
                case "HFMA2":
                    PackedHalf(context, inst);
                    break;

                case "F2FP": F2fp(context, inst); break;
                case "SUST": Sust(context, inst); break;
                case "SURED": Sured(context, inst); break;
                case "SULD": Suld(context, inst); break;
                case "HMNMX2": Hmnmx2(context, inst); break;
                case "HSETP2": Hsetp2(context, inst); break;
                case "MOVM": Movm(context, inst); break;
                case "HMMA": Hmma(context, inst); break;
                case "LDSM": Ldsm(context, inst); break;
                case "IABS": Iabs(context, inst); break;

                case "LDC":
                    Ldc(context, inst);
                    break;

                case "ULDC":
                    throw U(inst);

                case "AST":
                    Ast(context, inst);
                    break;

                case "ALD":
                    Ald(context, inst);
                    break;

                case "IPA":
                    Ipa(context, inst);
                    break;

                case "KILL":
                    context.Discard();
                    break;

                case "BSSY":
                {
                    // BSSY Bn, `(L): the threads that reach BSYNC Bn reconverge at L
                    st.BarrierTarget[BarrierIndex(inst.Ops[0])] = inst.Ops[1][2..^1];
                    break;
                }

                case "BSYNC":
                {
                    if (!st.BarrierTarget.TryGetValue(BarrierIndex(inst.Ops[0]), out string target))
                    {
                        throw new Sm86Unsupported("BSYNC without BSSY: " + inst.Text);
                    }

                    Branch(st, inst, target);
                    break;
                }

                case "EXIT":
                {
                    Operand g = Guard(context, inst);

                    if (inst.Ops.Length > 0 && IsPredOperand(inst.Ops[0]))
                    {
                        // EXIT Pq: the guard and the predicate operand both have to hold (`@P0 EXIT P1`)
                        Operand q = Pred(context, inst.Ops[0]);
                        g = g == null ? q : context.BitwiseAnd(g, q);
                    }

                    if (g == null)
                    {
                        if (context.PrepareForReturn())
                        {
                            context.Return();
                        }
                    }
                    else
                    {
                        Operand skip = Label();
                        context.BranchIfFalse(skip, g);

                        if (context.PrepareForReturn())
                        {
                            context.Return();
                        }

                        context.MarkLabel(skip);
                    }

                    break;
                }

                case "CALL":
                {
                    // CALL.REL.NOINC `(L): the compiler's own way to reach a reconvergence point (312 of 314 target a BSYNC): nothing is pushed, so it is a jump
                    if (!inst.Has("NOINC") || inst.Ops.Length != 1 || !inst.Ops[0].StartsWith("`(.L_x_"))
                    {
                        throw U(inst);
                    }

                    Branch(st, inst, inst.Ops[0][2..^1]);
                    break;
                }

                case "BRX":
                case "BRXU":        // the same jump through a uniform register (BRXU UR4 -0x240)
                    Brx(st, inst);
                    break;

                case "RET":
                    Ret(st, inst);
                    break;

                case "BRA":
                {
                    // BRA `(L) or BRA Pc, `(L): with a predicate operand the branch is taken when the guard and Pc both hold
                    string tgt = inst.Ops[^1];

                    if (inst.Ops.Length is < 1 or > 2 || !tgt.StartsWith("`(.L_x_") || (inst.Ops.Length == 2 && !IsPredOperand(inst.Ops[0])))
                    {
                        throw U(inst);
                    }

                    Branch(st, inst, tgt[2..^1], inst.Ops.Length == 2 ? Pred(context, inst.Ops[0]) : null);
                    break;
                }

                default:
                    throw U(inst);
            }
        }

        // BRX Rn -0x4ce0: an indirect jump (a switch). The register holds a table entry the shader loaded from c[1]; the target is entry + immediate + the address of the next instruction,
        // and the entries are instruction addresses, so the immediate is minus that address. The table (address, entries) comes from the control section (Sm86Translator.JumpTables):
        // the jump is a chain of comparisons with each distinct entry, the last one taken when nothing else matched
        private static void Brx(State st, SassInst inst)
        {
            EmitterContext context = st.Context;

            if (inst.Pred != 7)
            {
                throw U(inst);
            }

            string[] parts = inst.Ops.Length == 1 ? inst.Ops[0].Split(' ', StringSplitOptions.RemoveEmptyEntries) : inst.Ops;

            if (parts.Length != 2 || !TryConst(parts[1], out int imm) || !st.JumpTables.TryGetValue(inst.Address, out int[] targets) || targets.Length == 0)
            {
                throw new Sm86Unsupported("BRX without a jump table");
            }

            Operand value = context.IAdd(Src(context, parts[0], false), Const(imm + inst.Address + 0x10));
            List<int> distinct = new();

            foreach (int t in targets)
            {
                if (!distinct.Contains(t))
                {
                    distinct.Add(t);
                }
            }

            for (int i = 0; i < distinct.Count; i++)
            {
                if (!st.LabelAt.TryGetValue(distinct[i], out string name))
                {
                    throw new Sm86Unsupported("BRX target without a label");
                }

                if (i == distinct.Count - 1)
                {
                    context.Branch(st.LabelOf(name));
                }
                else
                {
                    context.BranchIfTrue(st.LabelOf(name), context.ICompareEqual(value, Const(distinct[i])));
                }
            }
        }

        // RET.REL.NODEC Rk: a jump to the address Rk holds, one of the returns the translator found for it
        private static void Ret(State st, SassInst inst)
        {
            EmitterContext context = st.Context;

            if (inst.Pred != 7 || !inst.Has("REL") || !st.JumpTables.TryGetValue(inst.Address, out int[] targets) || targets.Length == 0)
            {
                throw U(inst);
            }

            Operand value = Src(context, inst.Ops[0].Split(' ')[0], false);
            List<int> distinct = new();

            foreach (int t in targets)
            {
                if (!distinct.Contains(t))
                {
                    distinct.Add(t);
                }
            }

            for (int i = 0; i < distinct.Count; i++)
            {
                string name = st.LabelAt[distinct[i]];

                if (i == distinct.Count - 1)
                {
                    context.Branch(st.LabelOf(name));
                }
                else
                {
                    context.BranchIfTrue(st.LabelOf(name), context.ICompareEqual(value, Const(distinct[i])));
                }
            }
        }

        // Sm86Unsupported for an instruction: named by its mnemonic and first modifiers, so a census counts the forms and not the registers
        private static Sm86Unsupported U(SassInst inst) => new(inst.Name + (inst.Mods.Length > 0 ? "." + string.Join('.', inst.Mods, 0, Math.Min(2, inst.Mods.Length)) : ""));

        private static void Branch(State st, SassInst inst, string label, Operand extra = null)
        {
            Operand target = st.LabelOf(label);
            Operand g = Guard(st.Context, inst);

            if (extra != null)
            {
                g = g == null ? extra : st.Context.BitwiseAnd(g, extra);
            }

            if (g == null)
            {
                st.Context.Branch(target);
            }
            else
            {
                st.Context.BranchIfTrue(target, g);
            }
        }

        private static int BarrierIndex(string s)
        {
            if (s.Length > 1 && s[0] == 'B' && int.TryParse(s.AsSpan(1), out int n))
            {
                return n;
            }

            throw new Sm86Unsupported("barrier register: " + s);
        }

        // ---------------------------------------------------------------------------------------------------------------------------------------------------------------- integer ALU

        private static void Imad(EmitterContext context, SassInst inst)
        {
            Operand a = Src(context, inst.Ops[1], false), b = Src(context, inst.Ops[2], false);

            if (inst.Has("HI"))
            {
                // IMAD.HI[.U32] Rd, Ra, Rb, Rc: the high word of Ra * Rb, plus Rc
                Operand hi = inst.Has("U32") ? context.MultiplyHighU32(a, b) : context.MultiplyHighS32(a, b);
                Operand hc = Src(context, inst.Ops[3], false);
                SetReg(context, inst.Ops[0], IsZero(hc) ? hi : context.IAdd(hi, hc));
                return;
            }

            if (inst.Has("X"))
            {
                // IMAD.X Rd, Ra, Rb, Rc, Pcarry: Ra * Rb + Rc + the carry predicate
                Operand xp = IsZero(a) || IsZero(b) ? null : context.IMultiply(a, b);
                Operand xc = Src(context, inst.Ops[3], false);
                Operand sum = xp == null ? xc : IsZero(xc) ? xp : context.IAdd(xp, xc);
                SetReg(context, inst.Ops[0], context.IAdd(sum, Bits(context, Pred(context, inst.Ops[4]))));
                return;
            }

            if (inst.Has("WIDE"))
            {
                // Rd:Rd+1 = Ra * Rb + Rc:Rc+1 (unsigned 32x32 -> 64 bits, signed with .S32 or when neither is given)
                bool unsigned = inst.Has("U32");
                Operand lo = context.IMultiply(a, b);
                Operand hi = unsigned ? context.MultiplyHighU32(a, b) : context.MultiplyHighS32(a, b);
                (Operand cLo, Operand cHi) = Src64(context, inst.Ops[3]);
                Operand sumLo = context.IAdd(lo, cLo);
                Operand carry = context.ConditionalSelect(context.ICompareLessUnsigned(sumLo, lo), Const(1), Const(0));
                Operand sumHi = context.IAdd(context.IAdd(hi, cHi), carry);
                SetReg(context, inst.Ops[0], sumLo);
                SetReg(context, RegName(inst.Ops[0], 1), sumHi);
                return;
            }

            Operand c = Src(context, inst.Ops[3], false);
            Operand product = IsZero(a) || IsZero(b) ? null : context.IMultiply(a, b);
            SetReg(context, inst.Ops[0], product == null ? c : IsZero(c) ? product : context.IAdd(product, c));
        }

        // A 64-bit source: a register pair, a constant (the high word the sign for IMAD.WIDE.S32 is not needed with RZ), or zero
        private static (Operand, Operand) Src64(EmitterContext context, string s)
        {
            if (s.StartsWith("R"))
            {
                int r = RegIndex(s);

                return r == RZ ? (Const(0), Const(0)) : (Register(r, RegisterType.Gpr), r + 1 >= RZ ? Const(0) : Register(r + 1, RegisterType.Gpr));
            }

            Operand lo = Src(context, s, false);

            return (lo, Const(0));
        }

        private static void Iadd3(EmitterContext context, SassInst inst)
        {
            // IADD3[.X] Rd, a, b, c [, carry-in predicates]: carry-out destinations are not handled
            int first = 1;

            if (IsPredOperand(inst.Ops[1]))
            {
                Iadd3Carry(context, inst);
                return;
            }

            Operand sum = context.IAdd(context.IAdd(Src(context, inst.Ops[first], false), Src(context, inst.Ops[first + 1], false)), Src(context, inst.Ops[first + 2], false));

            if (inst.Has("X"))
            {
                for (int i = first + 3; i < inst.Ops.Length; i++)
                {
                    sum = context.IAdd(sum, context.ConditionalSelect(Pred(context, inst.Ops[i]), Const(1), Const(0)));
                }
            }

            SetReg(context, inst.Ops[0], sum);
        }

        private static void Lea(EmitterContext context, SassInst inst)
        {
            // LEA Rd, Ra, Rb, shift: (Ra << shift) + Rb
            if (inst.Has("HI") || inst.Has("X") || inst.Ops.Length != 4 || IsPredOperand(inst.Ops[1]))
            {
                LeaFull(context, inst);
                return;
            }

            Operand a = Src(context, inst.Ops[1], false), b = Src(context, inst.Ops[2], false), sh = Src(context, inst.Ops[3], false);
            SetReg(context, inst.Ops[0], context.IAdd(context.ShiftLeft(a, sh), b));
        }

        private static void Lop3(EmitterContext context, SassInst inst)
        {
            // LOP3.LUT Rd, a, b, c, lut, !PT   or   LOP3.LUT Pd, Rd, a, b, c, lut, !PT (the predicate is set when the result is not zero)
            bool pred = inst.Ops[0].StartsWith("P") || inst.Ops[0].StartsWith("UP");
            int o = pred ? 2 : 1;
            Operand a = Src(context, inst.Ops[o], false), b = Src(context, inst.Ops[o + 1], false), c = Src(context, inst.Ops[o + 2], false);

            if (!TryConst(inst.Ops[o + 3], out int lut))
            {
                throw U(inst);
            }

            Operand res = Lop3Expression.GetFromTruthTable(context, a, b, c, lut & 0xff);

            if (pred)
            {
                SetPred(context, inst.Ops[0], context.ICompareNotEqual(res, Const(0)));

                if (inst.Ops[1] != "RZ")
                {
                    SetReg(context, inst.Ops[1], res);
                }
            }
            else
            {
                SetReg(context, inst.Ops[0], res);
            }
        }

        private static void Shf(EmitterContext context, SassInst inst)
        {
            // SHF.<L|R>.<U32|S32>[.HI] Rd, Ra, shift, Rc: a funnel shift of Rc:Ra (Rc the high word); without .HI the low word of the result, with it the high word
            bool left = inst.Has("L"), hi = inst.Has("HI"), signed = inst.Has("S32") || inst.Has("S64");
            Operand lo = Src(context, inst.Ops[1], false), sh = Src(context, inst.Ops[2], false), high = Src(context, inst.Ops[3], false);

            if (inst.Has("S64") || inst.Has("U64"))
            {
                throw U(inst);
            }

            // Shifts of 32 and more give nothing (or the sign) rather than wrapping: the amount is masked to 5 bits here, which holds for the constant amounts the compiler emits
            Operand s = context.BitwiseAnd(sh, Const(31));
            Operand inv = context.ISubtract(Const(32), s);
            Operand zero = context.ICompareEqual(s, Const(0));
            Operand res;

            if (left)
            {
                res = hi ? context.ConditionalSelect(zero, high, context.BitwiseOr(context.ShiftLeft(high, s), context.ShiftRightU32(lo, inv)))
                         : context.ShiftLeft(lo, s);
            }
            else
            {
                Operand shifted = signed ? context.ShiftRightS32(high, s) : context.ShiftRightU32(high, s);
                res = hi ? shifted : context.ConditionalSelect(zero, lo, context.BitwiseOr(context.ShiftRightU32(lo, s), context.ShiftLeft(high, inv)));
            }

            SetReg(context, inst.Ops[0], res);
        }

        // ---------------------------------------------------------------------------------------------------------------------------------------------------------------- comparisons

        private static string[] CmpNames = { "F", "LT", "EQ", "LE", "GT", "NE", "GE", "NUM", "NAN", "LTU", "EQU", "LEU", "GTU", "NEU", "GEU", "T" };

        private static Operand FpCompare(EmitterContext context, string cond, Operand a, Operand b)
        {
            switch (cond)
            {
                case "T": return Const(IrConsts.True);
                case "F": return Const(IrConsts.False);
                case "NUM": return context.BitwiseNot(context.BitwiseOr(context.IsNan(a), context.IsNan(b)));
                case "NAN": return context.BitwiseOr(context.IsNan(a), context.IsNan(b));
            }

            bool unordered = cond.EndsWith('U');
            string baseCond = unordered ? cond[..^1] : cond;
            Instruction inst = baseCond switch
            {
                "LT" => Instruction.CompareLess,
                "EQ" => Instruction.CompareEqual,
                "LE" => Instruction.CompareLessOrEqual,
                "GT" => Instruction.CompareGreater,
                "NE" => Instruction.CompareNotEqual,
                "GE" => Instruction.CompareGreaterOrEqual,
                _ => throw new Sm86Unsupported("float comparison " + cond),
            };

            Operand res = context.Add(inst | Instruction.FP32, Local(), a, b);

            if (unordered)
            {
                res = context.BitwiseOr(res, context.BitwiseOr(context.IsNan(a), context.IsNan(b)));
            }

            return res;
        }

        private static Operand IntCompare(EmitterContext context, string cond, Operand a, Operand b, bool signed)
        {
            return cond switch
            {
                "EQ" => context.ICompareEqual(a, b),
                "NE" => context.ICompareNotEqual(a, b),
                "LT" => signed ? context.ICompareLess(a, b) : context.ICompareLessUnsigned(a, b),
                "LE" => signed ? context.ICompareLessOrEqual(a, b) : context.ICompareLessOrEqualUnsigned(a, b),
                "GT" => signed ? context.ICompareGreater(a, b) : context.ICompareGreaterUnsigned(a, b),
                "GE" => signed ? context.ICompareGreaterOrEqual(a, b) : context.ICompareGreaterOrEqualUnsigned(a, b),
                "T" => Const(IrConsts.True),
                "F" => Const(IrConsts.False),
                _ => throw new Sm86Unsupported("integer comparison " + cond),
            };
        }

        private static string FindCmp(SassInst inst)
        {
            foreach (string m in inst.Mods)
            {
                if (Array.IndexOf(CmpNames, m) >= 0)
                {
                    return m;
                }
            }

            throw U(inst);
        }

        private static Operand BoolOp(EmitterContext context, SassInst inst, Operand res, Operand pin)
        {
            return inst.Has("OR") ? context.BitwiseOr(res, pin) : inst.Has("XOR") ? context.BitwiseExclusiveOr(res, pin) : context.BitwiseAnd(res, pin);
        }

        private static void Isetp(EmitterContext context, SassInst inst)
        {
            // ISETP.<cmp>[.U32].<AND|OR|XOR>[.EX] Pd, Pd2, Ra, Rb, Pin
            if (inst.Has("EX"))
            {
                // The extended form compares the high words of a 64-bit value with the carry of the low compare: the predicate input holds that
                string cmp0 = FindCmp(inst);
                Operand a0 = Src(context, inst.Ops[2], false), b0 = Src(context, inst.Ops[3], false), prev = Pred(context, inst.Ops[5]);
                bool s0 = !inst.Has("U32");
                Operand eq = context.ICompareEqual(a0, b0);
                Operand res0 = cmp0 switch
                {
                    "EQ" => context.BitwiseAnd(eq, prev),
                    "NE" => context.BitwiseOr(context.ICompareNotEqual(a0, b0), prev),
                    _ => context.BitwiseOr(IntCompare(context, cmp0 is "LE" ? "LT" : cmp0 is "GE" ? "GT" : cmp0, a0, b0, s0), context.BitwiseAnd(eq, prev)),
                };
                SetPred(context, inst.Ops[0], context.BitwiseAnd(res0, Pred(context, inst.Ops[4])));
                SetPred(context, inst.Ops[1], context.BitwiseAnd(context.BitwiseNot(res0), Pred(context, inst.Ops[4])));
                return;
            }

            string cmp = FindCmp(inst);
            bool signed = !inst.Has("U32");
            Operand res = IntCompare(context, cmp, Src(context, inst.Ops[2], false), Src(context, inst.Ops[3], false), signed);
            Operand pin = Pred(context, inst.Ops[4]);
            SetPred(context, inst.Ops[0], BoolOp(context, inst, res, pin));
            SetPred(context, inst.Ops[1], BoolOp(context, inst, context.BitwiseNot(res), pin));
        }

        private static void Fsetp(EmitterContext context, SassInst inst)
        {
            Operand res = FpCompare(context, FindCmp(inst), Src(context, inst.Ops[2], true), Src(context, inst.Ops[3], true));
            Operand pin = Pred(context, inst.Ops[4]);
            SetPred(context, inst.Ops[0], BoolOp(context, inst, res, pin));
            SetPred(context, inst.Ops[1], BoolOp(context, inst, context.BitwiseNot(res), pin));
        }

        // HSETP2.<cmp>.<AND|OR> Pd, Pe, a, b, Pc (or a, b.lo, b.hi, Pc with two immediates): the low halves are compared into Pd and the high halves into Pe, each combined with Pc.
        // Both results are computed before either predicate is written: Pd may be Pc itself (HSETP2.LT.AND P0, PT, R14.H0_H0, R16.H0_H0, P0)
        private static void Hsetp2(EmitterContext context, SassInst inst)
        {
            int n = inst.Ops.Length;

            if ((n != 5 && n != 6) || inst.Has("H_AND") || inst.Has("BF"))
            {
                throw U(inst);
            }

            (Operand al, Operand ah) = HalfPair(context, inst.Ops[2]);
            (Operand bl, Operand bh) = n == 6 ? HalfImmPair(inst.Ops[3], inst.Ops[4]) : HalfPair(context, inst.Ops[3]);
            Operand pin = Pred(context, inst.Ops[n - 1]);
            Operand lo = BoolOp(context, inst, FpCompare(context, FindCmp(inst), al, bl), pin);
            Operand hi = BoolOp(context, inst, FpCompare(context, FindCmp(inst), ah, bh), pin);
            SetPred(context, inst.Ops[0], lo);
            SetPred(context, inst.Ops[1], hi);
        }

        private static void Fset(EmitterContext context, SassInst inst)
        {
            // FSET.BF.<cmp>.<AND|OR|XOR> Rd, Ra, Rb, Pin: 1.0 or 0.0 (with .BF) as the comparison says
            Operand res = FpCompare(context, FindCmp(inst), Src(context, inst.Ops[1], true), Src(context, inst.Ops[2], true));
            res = BoolOp(context, inst, res, Pred(context, inst.Ops[3]));
            SetReg(context, inst.Ops[0], inst.Has("BF") ? context.ConditionalSelect(res, ConstF(1f), Const(0)) : context.ConditionalSelect(res, Const(-1), Const(0)));
        }

        private static void Plop3(EmitterContext context, SassInst inst)
        {
            // PLOP3.LUT Pd, Pd2, Pa, Pb, Pc, lut1, lut2
            if (inst.Ops.Length != 7 || !TryConst(inst.Ops[5], out int lut1) || !TryConst(inst.Ops[6], out int lut2))
            {
                throw U(inst);
            }

            Operand a = Pred(context, inst.Ops[2]), b = Pred(context, inst.Ops[3]), c = Pred(context, inst.Ops[4]);
            SetPred(context, inst.Ops[0], context.ICompareNotEqual(Lop3Expression.GetFromTruthTable(context, Bits(context, a), Bits(context, b), Bits(context, c), lut1 & 0xff), Const(0)));
            SetPred(context, inst.Ops[1], context.ICompareNotEqual(Lop3Expression.GetFromTruthTable(context, Bits(context, a), Bits(context, b), Bits(context, c), lut2 & 0xff), Const(0)));
        }

        private static Operand Bits(EmitterContext context, Operand pred) => context.ConditionalSelect(pred, Const(1), Const(0));

        // ---------------------------------------------------------------------------------------------------------------------------------------------------------------- float

        private static void SetFp(EmitterContext context, SassInst inst, Operand r)
        {
            SetReg(context, inst.Ops[0], inst.Has("SAT") ? context.FPSaturate(r) : r);
        }

        private static void Mufu(EmitterContext context, SassInst inst)
        {
            Operand a = Src(context, inst.Ops[1], true);
            Operand r;

            if (inst.Has("RCP")) r = context.FPReciprocal(a);
            else if (inst.Has("RSQ")) r = context.FPReciprocalSquareRoot(a);
            else if (inst.Has("SQRT")) r = context.FPSquareRoot(a);
            else if (inst.Has("EX2")) r = context.FPExponentB2(a);
            else if (inst.Has("LG2")) r = context.FPLogarithmB2(a);
            else if (inst.Has("SIN")) r = context.FPSine(a);
            else if (inst.Has("COS")) r = context.FPCosine(a);
            else throw U(inst);

            SetFp(context, inst, r);
        }

        private static void F2i(EmitterContext context, SassInst inst)
        {
            // F2I.<FTZ>.<type>.<round>.NTZ Rd, Ra: the type is S32 unless U32, U16, S16, U8, S8 is given; the rounding is to nearest unless FLOOR, CEIL, TRUNC says otherwise
            Operand a = Src(context, inst.Ops[1], true);
            a = inst.Has("FLOOR") ? context.FPFloor(a) : inst.Has("CEIL") ? context.FPCeiling(a) : inst.Has("TRUNC") ? a : context.FPRound(a);
            bool unsigned = inst.Has("U32") || inst.Has("U16") || inst.Has("U8");
            Operand r = unsigned ? context.FP32ConvertToU32(a) : context.FP32ConvertToS32(a);

            if (inst.Has("U16")) r = context.IClampU32(r, Const(0), Const(0xffff));
            else if (inst.Has("S16")) r = context.IClampS32(r, Const(-0x8000), Const(0x7fff));
            else if (inst.Has("U8")) r = context.IClampU32(r, Const(0), Const(0xff));
            else if (inst.Has("S8")) r = context.IClampS32(r, Const(-0x80), Const(0x7f));

            SetReg(context, inst.Ops[0], r);
        }

        private static void I2f(EmitterContext context, SassInst inst)
        {
            // I2FP.F32.<S32|U32> Rd, Ra   and   I2F.<S16|U16|U8|S8> Rd, Ra[.H1|.B1|.B2|.B3]
            if (inst.Has("F64") || inst.Has("F16"))
            {
                throw U(inst);
            }

            (string reg, string sel) = SplitSelector(inst.Ops[1]);
            Operand a = Src(context, reg, false);

            if (inst.Has("U16") || inst.Has("S16"))
            {
                a = sel == "H1" ? context.ShiftRightU32(a, Const(16)) : a;
                a = inst.Has("U16") ? context.BitwiseAnd(a, Const(0xffff)) : context.BitfieldExtractS32(a, Const(0), Const(16));
            }
            else if (inst.Has("U8") || inst.Has("S8"))
            {
                int shift = sel != null && sel.Length == 2 && sel[0] == 'B' ? (sel[1] - '0') * 8 : 0;
                a = shift != 0 ? context.ShiftRightU32(a, Const(shift)) : a;
                a = inst.Has("U8") ? context.BitwiseAnd(a, Const(0xff)) : context.BitfieldExtractS32(a, Const(0), Const(8));
            }

            bool unsigned = inst.Has("U32") || inst.Has("U16") || inst.Has("U8");
            SetReg(context, inst.Ops[0], unsigned ? context.IConvertU32ToFP32(a) : context.IConvertS32ToFP32(a));
        }

        // A half-precision source: `R.H1_H1`, `-RZ`, `R.reuse.H0_H0`: the selected half, as a float
        private static Operand HalfSrc(EmitterContext context, string s)
        {
            bool neg = false;
            s = s.Trim();

            if (s.StartsWith('-'))
            {
                neg = true;
                s = s[1..];
            }

            (string reg, string sel) = SplitSelector(s);
            Operand v = Src(context, reg, false);
            Operand f = sel != null && sel.StartsWith("H1") ? context.UnpackHalf2x16High(v) : context.UnpackHalf2x16Low(v);

            return neg ? context.FPNegate(f) : f;
        }

        private static (string, string) SplitSelector(string s)
        {
            // R4.reuse.H1_H1 -> (R4, H1_H1); R4.B2 -> (R4, B2)
            string[] p = s.Split('.');
            string sel = null;

            for (int i = 1; i < p.Length; i++)
            {
                if (p[i] != "reuse")
                {
                    sel = p[i];
                }
            }

            return (p[0], sel);
        }

        // ---------------------------------------------------------------------------------------------------------------------------------------------------------------- memory

        private static void Ldc(EmitterContext context, SassInst inst)
        {
            // LDC[.64] Rd, c[slot][Rb + offset]
            string src = inst.Ops[1];

            if (!src.StartsWith("c[") || !TryParseCbuf(src, out int slot, out string indexReg, out int offset))
            {
                throw U(inst);
            }

            int count = inst.Has("64") ? 2 : 1;

            if (inst.Has("128"))
            {
                throw U(inst);
            }

            Operand addr = indexReg == null ? Const(offset) : context.IAdd(Src(context, indexReg, false), Const(offset));
            Operand word = context.ShiftRightU32(addr, Const(2));

            if (inst.Has("U8") || inst.Has("S8") || inst.Has("U16") || inst.Has("S16"))
            {
                // A byte or halfword of the word that holds it
                bool is8 = inst.Has("U8") || inst.Has("S8"), sgn = inst.Has("S8") || inst.Has("S16");
                Operand w = InstEmit.EmitLoadConstant(context, Const(slot), word);
                Operand sh = context.ShiftLeft(context.BitwiseAnd(addr, Const(is8 ? 3 : 2)), Const(3));
                SetReg(context, inst.Ops[0], sgn ? context.BitfieldExtractS32(w, sh, Const(is8 ? 8 : 16)) : context.BitfieldExtractU32(w, sh, Const(is8 ? 8 : 16)));
                return;
            }

            for (int i = 0; i < count; i++)
            {
                if (RegIndex(inst.Ops[0]) == RZ)
                {
                    break;
                }

                Operand v = InstEmit.EmitLoadConstant(context, Const(slot), context.IAdd(word, Const(i)));
                SetReg(context, RegName(inst.Ops[0], i), v);
            }
        }

        // AST.<bits> a[offset], Rs: stores consecutive registers to consecutive attribute words
        private static void Ast(EmitterContext context, SassInst inst)
        {
            string attr = inst.Ops[0];

            if (!attr.StartsWith("a[") || !TryConst(attr[2..^1], out int offset))
            {
                throw U(inst);
            }

            int src = RegIndex(inst.Ops[1]);
            int count = AccessWords(inst);

            for (int index = 0; index < count; index++)
            {
                int o = offset + index * 4;

                if (!context.TranslatorContext.AttributeUsage.IsUsedOutputAttribute(o) && o >= AttributeConsts.UserAttributeBase && o < AttributeConsts.UserAttributeEnd)
                {
                    continue;
                }

                o = InstEmit.FixedFuncToUserAttribute(context.TranslatorContext, o, isOutput: true);
                Operand value = src == RZ || src + index >= RZ ? Const(0) : Register(src + index, RegisterType.Gpr);
                AttributeMap.GenerateAttributeStore(context, o, false, value);
            }
        }

        // ---------------------------------------------------------------------------------------------------------------------------------------------------------------- operands

        private static bool IsZero(Operand o) => o.Type == OperandType.Constant && o.Value == 0;

        private static bool IsUniform(string s) => s.StartsWith("UR") || s == "URZ";

        // R12 / RZ / UR5 / URZ (with .reuse and the like dropped) -> the register index in the bank, RZ for the zero registers
        private static int RegIndex(string s)
        {
            s = s.Trim();

            int dot = s.IndexOf('.');
            if (dot > 0)
            {
                s = s[..dot];
            }

            if (s == "RZ" || s == "URZ")
            {
                return RZ;
            }

            if (s.StartsWith("UR") && int.TryParse(s.AsSpan(2), out int u))
            {
                return UrBase + u;
            }

            if (s.Length > 1 && s[0] == 'R' && int.TryParse(s.AsSpan(1), out int n))
            {
                return n;
            }

            throw new Sm86Unsupported("register operand: " + s);
        }

        // The name of the register `delta` places after s (R4 + 1 = R5)
        private static string RegName(string s, int delta)
        {
            if (delta == 0)
            {
                return s;
            }

            int i = RegIndex(s);

            return i == RZ ? "RZ" : i >= UrBase ? $"UR{i - UrBase + delta}" : $"R{i + delta}";
        }

        private static void SetReg(EmitterContext context, string dest, Operand value)
        {
            int rd = RegIndex(dest);

            if (rd != RZ)
            {
                context.Copy(Register(rd, RegisterType.Gpr), value);
            }
        }

        private static void SetPred(EmitterContext context, string dest, Operand value)
        {
            dest = dest.Trim();

            if (dest == "PT" || dest == "UPT")
            {
                return;
            }

            context.Copy(Register(PredIndex(dest), RegisterType.Predicate), value);
        }

        private static int PredIndex(string s)
        {
            s = s.TrimStart('!');

            if (s.StartsWith("UP") && int.TryParse(s.AsSpan(2), out int u))
            {
                return UpBase + u;
            }

            if (s.StartsWith("P") && int.TryParse(s.AsSpan(1), out int n))
            {
                return n;
            }

            throw new Sm86Unsupported("predicate operand: " + s);
        }

        // P0, !P0, PT, !PT, UP1, UPT
        private static Operand Pred(EmitterContext context, string s)
        {
            s = s.Trim();
            bool not = s.StartsWith('!');

            if (not)
            {
                s = s[1..];
            }

            Operand v = s == "PT" || s == "UPT" ? Const(IrConsts.True) : Register(PredIndex(s), RegisterType.Predicate);

            return not ? context.BitwiseNot(v) : v;
        }

        private static bool TryConst(string s, out int value)
        {
            s = s.Trim();
            bool neg = s.StartsWith('-');

            if (neg)
            {
                s = s[1..];
            }

            if (s.StartsWith("0x") && uint.TryParse(s.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out uint u))
            {
                value = neg ? -(int)u : (int)u;
                return true;
            }

            value = 0;
            return false;
        }

        // c[0x3][0x10], c[0x3][R5+0x10], c[0x3][R5]
        private static bool TryParseCbuf(string s, out int slot, out string indexReg, out int offset)
        {
            slot = 0; indexReg = null; offset = 0;
            int close = s.IndexOf(']');

            if (!s.StartsWith("c[") || close < 0 || s.Length < close + 4 || s[close + 1] != '[' || !TryConst(s[2..close], out slot))
            {
                return false;
            }

            string inner = s[(close + 2)..^1];
            int plus = inner.IndexOf('+');

            if (inner.StartsWith("R") || inner.StartsWith("UR"))
            {
                indexReg = plus < 0 ? inner : inner[..plus];

                if (plus >= 0)
                {
                    return TryConst(inner[(plus + 1)..], out offset);
                }

                return true;
            }

            return TryConst(inner, out offset);
        }

        // A source operand: a register (with | | and - modifiers), an immediate, or a constant buffer word
        private static Operand Src(EmitterContext context, string s, bool fp)
        {
            s = s.Trim();
            bool neg = false, abs = false, not = false;

            if (s.EndsWith(".reuse"))
            {
                s = s[..^6];
            }

            if (s.StartsWith('~'))
            {
                not = true;
                s = s[1..];
            }

            if (s.StartsWith('-'))
            {
                neg = true;
                s = s[1..];
            }

            if (s.StartsWith('|') && s.EndsWith('|'))
            {
                abs = true;
                s = s[1..^1];
            }

            Operand v;

            if (s.StartsWith("c["))
            {
                if (!TryParseCbuf(s, out int slot, out string indexReg, out int offset))
                {
                    throw new Sm86Unsupported("constant buffer operand: " + s);
                }

                if (indexReg == null)
                {
                    v = InstEmitHelper.GetSrcCbuf(context, slot, offset >> 2);
                }
                else
                {
                    v = InstEmit.EmitLoadConstant(context, Const(slot), context.ShiftRightU32(context.IAdd(Src(context, indexReg, false), Const(offset)), Const(2)));
                }
            }
            else if (s.StartsWith("R") || s.StartsWith("UR"))
            {
                int r = RegIndex(s);
                v = r == RZ ? Const(0) : Register(r, RegisterType.Gpr);
            }
            else if (s.StartsWith("0x"))
            {
                TryConst(s, out int imm);
                v = Const(imm);
            }
            else if (float.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out float f))
            {
                v = fp ? ConstF(f) : Const((int)f);
            }
            else if (s == "+INF" || s == "INF")
            {
                v = ConstF(float.PositiveInfinity);
            }
            else
            {
                throw new Sm86Unsupported("operand: " + s);
            }

            if (fp && (abs || neg))
            {
                v = context.FPAbsNeg(v, abs, neg);
            }
            else if (!fp && (abs || neg))
            {
                v = context.IAbsNeg(v, abs, neg);
            }

            return not ? context.BitwiseNot(v) : v;
        }
    }
}
