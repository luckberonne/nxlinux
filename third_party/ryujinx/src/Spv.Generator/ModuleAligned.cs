using static Spv.Specification;

namespace Spv.Generator
{
    public partial class Module
    {
        // OpLoad/OpStore with the Aligned memory operand, which Vulkan requires for PhysicalStorageBuffer pointers.
        public Instruction LoadAligned(Instruction resultType, Instruction pointer, uint alignment)
        {
            Instruction result = NewInstruction(Op.OpLoad, GetNewId(), resultType);

            result.AddOperand(pointer);
            result.AddOperand(MemoryAccessMask.Aligned);
            result.AddOperand((LiteralInteger)alignment);

            AddToFunctionDefinitions(result);

            return result;
        }

        public Instruction StoreAligned(Instruction pointer, Instruction obj, uint alignment)
        {
            Instruction result = NewInstruction(Op.OpStore);

            result.AddOperand(pointer);
            result.AddOperand(obj);
            result.AddOperand(MemoryAccessMask.Aligned);
            result.AddOperand((LiteralInteger)alignment);

            AddToFunctionDefinitions(result);

            return result;
        }
    }
}
