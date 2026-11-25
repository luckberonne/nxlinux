namespace Ryujinx.Graphics.Shader.Decoders
{
    static class RegisterConsts
    {
        public const int GprsCount = 255;
        public const int PredsCount = 7;
        public const int FlagsCount = 4;
        public const int TotalCount = SsaGprs + SsaPreds + FlagsCount;

        // The Sm86 frontend keeps the 63 uniform registers above RZ (index 256 + n) and the 7 uniform predicates above PT (index 8 + n): the SSA tables are sized for them
        public const int SsaGprs = GprsCount + 1 + 63;
        public const int SsaPreds = PredsCount + 1 + 7;

        public const int RegisterZeroIndex = GprsCount;
        public const int PredicateTrueIndex = PredsCount;
    }
}
