using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation.Sm86
{
    // IPA and ALD: the logic of Instructions/InstEmitAttribute.cs for the forms Ampere code uses (a constant attribute address, no indexing)
    static partial class Sm86Emitter
    {
        private static int AttrOffset(SassInst inst)
        {
            string a = Array.Find(inst.Ops, o => o.StartsWith("a["));

            if (a == null || !TryConst(a[2..^1], out int offset))
            {
                throw new Sm86Unsupported("attribute operand: " + inst.Text);
            }

            return offset;
        }

        // IPA.PASS[.CENTROID|.CONSTANT] Rd, a[offset]: the attribute as the pixel's interpolation gives it. As on Maxwell, a perspective attribute comes back divided by w (the host's
        // interpolation is perspective-correct already, so it is multiplied by gl_FragCoord.w), and the code multiplies by the reciprocal of a[0x7c] itself.
        private static void Ipa(EmitterContext context, SassInst inst)
        {
            int imm = AttrOffset(inst);
            bool isFixedFunc = InstEmit.TryFixedFuncToUserAttributeIpa(context, imm, out Operand res);

            if (imm is >= AttributeConsts.UserAttributeBase and < AttributeConsts.UserAttributeEnd)
            {
                int index = (imm - AttributeConsts.UserAttributeBase) >> 4;

                if (!inst.Has("CONSTANT") && context.TranslatorContext.Definitions.ImapTypes[index].GetFirstUsedType() == PixelImap.Perspective)
                {
                    res = context.FPMultiply(res, context.Load(StorageKind.Input, IoVariable.FragmentCoord, null, Const(3)));
                }
            }
            else if (imm is AttributeConsts.PositionX or AttributeConsts.PositionY)
            {
                res = context.FPDivide(res, context.Load(StorageKind.ConstantBuffer, SupportBuffer.Binding, Const((int)SupportBufferField.RenderScale), Const(0)));

                if (imm == AttributeConsts.PositionY && context.TranslatorContext.Options.TargetApi != TargetApi.OpenGL && context.TranslatorContext.Definitions.YNegateEnabled)
                {
                    Operand viewportHeight = context.Load(StorageKind.ConstantBuffer, 0, Const((int)SupportBufferField.ViewportSize), Const(1));
                    res = context.FPSubtract(viewportHeight, res);
                }
            }
            else if (imm == AttributeConsts.PrimitiveId && context.TranslatorContext.Definitions.HalvePrimitiveId)
            {
                res = context.ShiftRightS32(res, Const(1));
            }

            _ = isFixedFunc;
            SetFp(context, inst, res);
        }

        // ALD[.64|.96|.128] Rd, a[offset]: a vertex shader's input attributes (or a stage's inputs)
        private static void Ald(EmitterContext context, SassInst inst)
        {
            int offset0 = AttrOffset(inst);
            int count = AccessWords(inst);

            for (int index = 0; index < count; index++)
            {
                string rd = RegName(inst.Ops[0], index);

                if (RegIndex(rd) == RZ)
                {
                    break;
                }

                int offset = InstEmit.FixedFuncToUserAttribute(context.TranslatorContext, offset0 + index * 4, false);
                Operand value;

                if (InstEmit.TryConvertIdToIndexForVulkan(context, offset, out value))
                {
                    SetReg(context, rd, value);
                    continue;
                }

                value = AttributeMap.GenerateAttributeLoad(context, null, offset, false, false);

                if ((!context.TranslatorContext.Definitions.SupportsScaledVertexFormats || context.VertexAsCompute) &&
                    context.TranslatorContext.Stage == ShaderStage.Vertex && offset >= 0x80 && offset < 0x280)
                {
                    AttributeType type = context.TranslatorContext.Definitions.GetAttributeType((offset - 0x80) >> 4);

                    if (type == AttributeType.Sscaled)
                    {
                        value = context.IConvertS32ToFP32(value);
                    }
                    else if (type == AttributeType.Uscaled)
                    {
                        value = context.IConvertU32ToFP32(value);
                    }
                }
                else if (offset == AttributeConsts.PrimitiveId && context.TranslatorContext.Definitions.HalvePrimitiveId)
                {
                    value = context.ShiftRightS32(value, Const(1));
                }

                SetReg(context, rd, value);
            }
        }
    }
}
