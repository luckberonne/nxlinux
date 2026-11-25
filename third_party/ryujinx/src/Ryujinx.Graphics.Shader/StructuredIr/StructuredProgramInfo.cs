using System.Collections.Generic;

namespace Ryujinx.Graphics.Shader.StructuredIr
{
    class StructuredProgramInfo
    {
        public List<StructuredFunction> Functions { get; }

        public HashSet<IoDefinition> IoDefinitions { get; }

        public HelperFunctionsMask HelperFunctionsMask { get; set; }

        public bool UsesPhysicalMemory { get; set; }

        public bool UsesSubgroupArithmetic { get; set; }

        public bool UsesAtomicFloatAdd { get; set; }

        public StructuredProgramInfo()
        {
            Functions = new List<StructuredFunction>();

            IoDefinitions = new HashSet<IoDefinition>();
        }
    }
}
