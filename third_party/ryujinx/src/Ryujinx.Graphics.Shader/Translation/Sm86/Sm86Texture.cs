using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using System.Numerics;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Sm86
{
    // Texture sampling. Operand layout (read off nvdisasm's output for CUDA texture fetches, whose SASS is the same dialect):
    //
    //   TEX.SCR[.LL|.LZ|.LB][.DC][.NODEP] Rhi, Rlo, Ra, Rb, <cbuf>, <word>, <dim>[, <mask>]       (bound: the handle is the word at c[cbuf][4 * word])
    //   TEX.SCR.B[.DC] Rhi, Rlo, Ra, Rb, <dim>[, <mask>]                                          (bindless: the handle is Rb's first register)
    //
    // The sources are one sequence: the array layer (arrays only), the coordinates, then the extras in this order: lod or bias, packed offsets, depth reference. Ra holds the first two
    // elements of the coordinate part, Rb the rest of it followed by the extras. The results are the components of the mask in ascending order (RGBA = bits 0..3, the whole of RGBA
    // without a mask): the first two go to Rlo, Rlo + 1 (the second register printed) and the next two to Rhi, Rhi + 1 (the first printed), RZ meaning the result is dropped.
    //
    // The bound form's two immediates are the constant buffer slot and the word offset: with c[2] and offset 8 + 2 n they are the combined-handle table of the NVN driver, as on
    // Maxwell (src/gfx/nvn/dmsh_abi.h: c[2] byte 0x20 + 8 n), so the binding is the one a Maxwell bindless read of that word would produce.
    static partial class Sm86Emitter
    {
        private static SamplerType ParseDim(string s, SassInst inst)
        {
            return s switch
            {
                "1D" => SamplerType.Texture1D,
                "2D" => SamplerType.Texture2D,
                "3D" => SamplerType.Texture3D,
                "CUBE" => SamplerType.TextureCube,
                "ARRAY_1D" => SamplerType.Texture1D | SamplerType.Array,
                "ARRAY_2D" => SamplerType.Texture2D | SamplerType.Array,
                "ARRAY_CUBE" => SamplerType.TextureCube | SamplerType.Array,
                _ => throw U(inst),
            };
        }

        // A register run (R12, R13, ...) handed out one at a time; past RZ it reads as zero
        private sealed class RegRun
        {
            private readonly EmitterContext _context;
            private int _index;

            public RegRun(EmitterContext context, string reg)
            {
                _context = context;
                _index = RegIndex(reg);
            }

            public Operand Next()
            {
                if (_index >= RZ)
                {
                    return Const(0);
                }

                return _context.Copy(Register(_index++, RegisterType.Gpr));
            }
        }

        private static void Tex(EmitterContext context, SassInst inst, bool gather, bool fetch = false)
        {
            // A gather's component is a modifier of its own (R, G, B or A): TLD4.SCR.B reads the blue component, it is not the bindless form (TLD4.SCR.G.B would be: R/G/A present)
            bool bflag = inst.Has("B") && !(gather && !inst.Has("R") && !inst.Has("G") && !inst.Has("A"));
            // TEX ..., UR10, 0x0, <dim>: the handle in a uniform register (plus an immediate offset, 0 in every shader seen): a bindless read whose handle is that register
            bool uniformHandle = !bflag && inst.Ops.Length > 5 && inst.Ops[4].StartsWith("UR");

            if (uniformHandle && (!TryConst(inst.Ops[5], out int uoff) || uoff != 0))
            {
                throw U(inst);
            }

            bool bindless = bflag || uniformHandle;
            bool dc = inst.Has("DC");
            bool lodLevel = inst.Has("LL"), lodZero = inst.Has("LZ"), lodBias = inst.Has("LB");

            bool aoffi = inst.Has("AOFFI"), ptp = inst.Has("PTP");

            // AOFFI: one packed register of per-coordinate offsets (4 bits each, a byte apart); PTP (a gather only): two registers of four offset pairs (6 bits each, a byte apart), one pair
            // for each texel the gather returns. Both registers are part of Rb's share of the hardware sequence, after the lod and before the depth reference
            if (inst.Has("MS") || inst.Has("LBA") || inst.Has("LLA") || inst.Has("CL") || inst.Has("INDEXED") || (ptp && (!gather || aoffi)) || (bindless && (aoffi || ptp)))
            {
                throw U(inst);
            }

            int dimAt = bflag ? 4 : 6;

            if (inst.Ops.Length <= dimAt)
            {
                throw U(inst);
            }

            SamplerType type = ParseDim(inst.Ops[dimAt], inst);
            int mask = 0xf;

            if (inst.Ops.Length > dimAt + 1 && !TryConst(inst.Ops[dimAt + 1], out mask))
            {
                throw U(inst);
            }

            int component = 0;

            if (gather)
            {
                component = inst.Has("R") ? 0 : inst.Has("G") ? 1 : inst.Has("B") ? 2 : inst.Has("A") ? 3 : throw U(inst);
            }

            int cbufSlot = 0, wordOffset = 0;

            if (!bindless && (!TryConst(inst.Ops[4], out cbufSlot) || !TryConst(inst.Ops[5], out wordOffset)))
            {
                throw U(inst);        // a uniform register in place of the constant buffer slot
            }

            bool isArray = type.HasFlag(SamplerType.Array);
            int coordsCount = type.Dimensions;

            // The hardware sequence, in registers
            RegRun ra = new(context, inst.Ops[2]);
            RegRun rb = new(context, inst.Ops[3]);

            // Ra and Rb split the sequence between them: Ra takes the first ceil(n / 2) elements, Rb the rest (n counts the layer, the coordinates, the lod or bias and the depth reference).
            // A 2D read without a lod is one element each (TEX.SCR.LZ R5, R6, R2, R9: x in R2, y in R9, not R2 and R3), a 3D one or a 2D one with a lod two in Ra and one in Rb
            int elements = (type.HasFlag(SamplerType.Array) ? 1 : 0) + type.Dimensions + (lodLevel || lodBias ? 1 : 0) + (aoffi ? 1 : 0) + (ptp ? 2 : 0) + (dc ? 1 : 0);
            int fromRa = bindless ? 2 : (elements + 1) / 2;

            Operand Take()
            {
                return fromRa-- > 0 ? ra.Next() : rb.Next();
            }

            List<Operand> sources = new();
            Operand arrayIndex = isArray ? Take() : null;
            Operand[] coords = new Operand[coordsCount];

            // For a bindless read the handle comes first from Rb, ahead of everything that Rb holds; the coordinates that spill into Rb follow it
            Operand bindlessHandle = null;

            if (uniformHandle)
            {
                bindlessHandle = Src(context, inst.Ops[4], false);
            }
            else if (bindless)
            {
                bindlessHandle = rb.Next();
            }

            for (int i = 0; i < coordsCount; i++)
            {
                coords[i] = Take();
            }

            TextureFlags flags = TextureFlags.None;

            if (bindless)
            {
                sources.Add(bindlessHandle);
                flags |= TextureFlags.Bindless;
            }

            sources.AddRange(coords);

            if (isArray)
            {
                sources.Add(arrayIndex);
            }

            Operand lodValue = lodLevel || lodBias ? rb.Next() : fetch ? Const(0) : ConstF(0);
            Operand[] packedOffs = new Operand[2];

            if (aoffi || ptp)
            {
                packedOffs[0] = rb.Next();
                packedOffs[1] = ptp ? rb.Next() : null;
            }

            void AddOffsets()
            {
                if (ptp)
                {
                    for (int index = 0; index < coordsCount * 4; index++)
                    {
                        sources.Add(context.BitfieldExtractS32(packedOffs[(index >> 2) & 1], Const((index & 3) * 8), Const(6)));
                    }

                    flags |= TextureFlags.Offsets;
                }
                else if (aoffi)
                {
                    for (int index = 0; index < coordsCount; index++)
                    {
                        sources.Add(context.BitfieldExtractS32(packedOffs[0], Const(index * 4), Const(4)));
                    }

                    flags |= TextureFlags.Offset;
                }
            }

            if (gather)
            {
                flags |= TextureFlags.Gather;

                if (dc)
                {
                    sources.Add(rb.Next());
                    type |= SamplerType.Shadow;
                }

                AddOffsets();

                if (!dc)
                {
                    sources.Add(Const(component));
                }
            }
            else
            {
                if (dc)
                {
                    sources.Add(rb.Next());
                    type |= SamplerType.Shadow;
                }

                if (lodLevel || lodZero)
                {
                    sources.Add(lodLevel ? lodValue : fetch ? Const(0) : ConstF(0));
                    flags |= TextureFlags.LodLevel;
                }

                if (fetch)
                {
                    flags |= TextureFlags.IntCoords;
                }

                AddOffsets();

                if (lodBias)
                {
                    sources.Add(lodValue);
                    flags |= TextureFlags.LodBias;
                }
            }

            // The results: the enabled components in ascending order, to Rlo, Rlo + 1, Rhi, Rhi + 1
            int hi = RegIndex(inst.Ops[0]), lo = RegIndex(inst.Ops[1]);
            int[] slots = { lo, lo + 1, hi, hi + 1 };
            int[] slotsOk = { lo, lo, hi, hi };
            int count = BitOperations.PopCount((uint)mask);
            Operand[] dests = new Operand[count];
            int k = 0;

            bool f16 = inst.Has("F16");

            for (int bit = 0; bit < 4; bit++)
            {
                if ((mask & (1 << bit)) == 0)
                {
                    continue;
                }

                bool dropped = slotsOk[k] == RZ || slots[k] >= RZ;
                dests[k] = dropped || f16 ? Local() : Register(slots[k], RegisterType.Gpr);
                k++;
            }

            // The bound form registers the texture under the handle word's address; the bindless form leaves the handle as a source for the translator's bindless pass
            SetBindingPair setAndBinding = default;

            if (!bindless)
            {
                int packed = TextureHandle.PackOffsets(wordOffset, 0, TextureHandleType.CombinedSampler);
                setAndBinding = context.ResourceManager.GetTextureOrImageBinding(Instruction.TextureSample, type, TextureFormat.Unknown, flags, cbufSlot, packed);
            }

            context.TextureSample(type, flags, setAndBinding, mask, dests, sources.ToArray());

            if (f16)
            {
                // .F16: the results come as packed halves, the first two components in Rlo and the next two in Rhi
                Operand zero = ConstF(0f);
                Operand p0 = context.PackHalf2x16(dests[0], count > 1 ? dests[1] : zero);
                SetReg(context, inst.Ops[1], p0);

                if (count > 2)
                {
                    SetReg(context, inst.Ops[0], context.PackHalf2x16(dests[2], count > 3 ? dests[3] : zero));
                }
            }
        }
        // TXD Rhi, Rlo, Ra, Rb, <cbuf>, <word>, <dim>[, <mask>]: a read with explicit derivatives (read off a CUDA tex.grad.2d / tex.grad.a2d: the other shapes it lowers to TEX). Ra holds the
        // coordinates, Rb the four derivatives interleaved by coordinate (dsdx, dsdy, dtdx, dtdy), and for an array the layer is in Rlo, which is also where the second pair of results
        // goes. The results are placed as TEX places them
        private static void Txd(EmitterContext context, SassInst inst)
        {
            if (inst.Has("B") || inst.Has("AOFFI") || inst.Has("MS") || inst.Has("LL") || inst.Has("LZ") || inst.Has("DC") || inst.Ops.Length < 7 ||
                !TryConst(inst.Ops[4], out int cbufSlot) || !TryConst(inst.Ops[5], out int wordOffset))
            {
                throw U(inst);
            }

            SamplerType type = ParseDim(inst.Ops[6], inst);
            int mask = 0xf;

            if (inst.Ops.Length > 7 && !TryConst(inst.Ops[7], out mask))
            {
                throw U(inst);
            }

            if (type.Dimensions != 2)
            {
                throw U(inst);
            }

            bool isArray = type.HasFlag(SamplerType.Array);
            RegRun ra = new(context, inst.Ops[2]);
            RegRun rb = new(context, inst.Ops[3]);

            // The layer and the derivatives are read before any result is written
            Operand layer = isArray ? Src(context, inst.Ops[1], false) : null;
            Operand s = ra.Next(), t = ra.Next();
            Operand dsdx = rb.Next(), dsdy = rb.Next(), dtdx = rb.Next(), dtdy = rb.Next();

            List<Operand> sources = new() { s, t };

            if (isArray)
            {
                sources.Add(layer);
            }

            sources.AddRange(new[] { dsdx, dtdx, dsdy, dtdy });

            int hi = RegIndex(inst.Ops[0]), lo = RegIndex(inst.Ops[1]);
            int[] slots = { lo, lo + 1, hi, hi + 1 };
            int[] slotsOk = { lo, lo, hi, hi };
            Operand[] dests = new Operand[BitOperations.PopCount((uint)mask)];
            int k = 0;

            for (int bit = 0; bit < 4; bit++)
            {
                if ((mask & (1 << bit)) == 0)
                {
                    continue;
                }

                bool dropped = slotsOk[k] == RZ || slots[k] >= RZ;
                dests[k] = dropped ? Local() : Register(slots[k], RegisterType.Gpr);
                k++;
            }

            TextureFlags flags = TextureFlags.Derivatives;
            int packed = TextureHandle.PackOffsets(wordOffset, 0, TextureHandleType.CombinedSampler);
            SetBindingPair setAndBinding = context.ResourceManager.GetTextureOrImageBinding(Instruction.TextureSample, type, TextureFormat.Unknown, flags, cbufSlot, packed);

            context.TextureSample(type, flags, setAndBinding, mask, dests, sources.ToArray());
        }
        // The registers a texture instruction's results go to: the components of the mask in ascending order, first Rlo, Rlo + 1 and then Rhi, Rhi + 1 (a Local when that result is dropped)
        private static Operand[] ResultRegisters(SassInst inst, int mask)
        {
            int hi = RegIndex(inst.Ops[0]), lo = RegIndex(inst.Ops[1]);
            int[] slots = { lo, lo + 1, hi, hi + 1 };
            int[] slotsOk = { lo, lo, hi, hi };
            Operand[] dests = new Operand[BitOperations.PopCount((uint)mask)];
            int k = 0;

            for (int bit = 0; bit < 4; bit++)
            {
                if ((mask & (1 << bit)) == 0)
                {
                    continue;
                }

                bool dropped = k >= 4 || slotsOk[k] == RZ || slots[k] >= RZ;
                dests[k] = dropped ? Local() : Register(slots[k], RegisterType.Gpr);
                k++;
            }

            return dests;
        }

        // TXQ Rhi, Rlo, Ra, TEX_HEADER_DIMENSION, <cbuf>, <word>, <mask>: the size of a texture at the level in Ra (width, height, depth = components 0, 1, 2 of the mask)
        private static void Txq(EmitterContext context, SassInst inst)
        {
            if (inst.Ops.Length != 7 || inst.Ops[3] != "TEX_HEADER_DIMENSION" || !TryConst(inst.Ops[4], out int cbufSlot) || !TryConst(inst.Ops[5], out int wordOffset) || !TryConst(inst.Ops[6], out int mask))
            {
                throw U(inst);
            }

            SamplerType type = (mask & 4) != 0 ? SamplerType.Texture3D : SamplerType.Texture2D;
            Operand lod = context.Copy(Src(context, inst.Ops[2], false));       // a copy: the result registers may include the one the level comes from (TXQ RZ, R2, R2), and the second component must still query that level
            Operand[] dests = ResultRegisters(inst, mask);

            int packed = TextureHandle.PackOffsets(wordOffset, 0, TextureHandleType.CombinedSampler);
            SetBindingPair setAndBinding = context.ResourceManager.GetTextureOrImageBinding(Instruction.TextureQuerySize, type, TextureFormat.Unknown, TextureFlags.None, cbufSlot, packed);
            int k = 0;

            for (int bit = 0; bit < 4; bit++)
            {
                if ((mask & (1 << bit)) != 0)
                {
                    context.Copy(dests[k++], context.TextureQuerySize(type, TextureFlags.None, setAndBinding, bit, new[] { lod }));
                }
            }
        }

        // TMML.LOD Rhi, Rlo, Ra, <cbuf>, <word>, <dim>, <mask>: the mip level the coordinates in Ra select, as 8.8 fixed point (the Maxwell encoding), component 0 and 1 of the mask
        private static void Tmml(EmitterContext context, SassInst inst)
        {
            if (!inst.Has("LOD") || inst.Ops.Length != 7 || !TryConst(inst.Ops[3], out int cbufSlot) || !TryConst(inst.Ops[4], out int wordOffset) || !TryConst(inst.Ops[6], out int mask))
            {
                throw U(inst);
            }

            SamplerType type = ParseDim(inst.Ops[5], inst);
            RegRun ra = new(context, inst.Ops[2]);
            List<Operand> sources = new();

            for (int i = 0; i < type.Dimensions; i++)
            {
                sources.Add(ra.Next());
            }

            Operand[] dests = ResultRegisters(inst, mask);
            int packed = TextureHandle.PackOffsets(wordOffset, 0, TextureHandleType.CombinedSampler);
            SetBindingPair setAndBinding = context.ResourceManager.GetTextureOrImageBinding(Instruction.Lod, type, TextureFormat.Unknown, TextureFlags.None, cbufSlot, packed);
            int k = 0;

            for (int bit = 0; bit < 4; bit++)
            {
                if ((mask & (1 << bit)) == 0)
                {
                    continue;
                }

                if (bit >= 2)
                {
                    context.Copy(dests[k++], Const(0));
                    continue;
                }

                // The instruction's component order is the inverse of GLSL's (as for Maxwell's TMML)
                Operand res = context.Lod(type, TextureFlags.None, setAndBinding, bit ^ 1, sources.ToArray());
                context.Copy(dests[k++], context.FP32ConvertToS32(context.FPMultiply(res, ConstF(256.0f))));
            }
        }
    }
}
