// The example host's self-test of the shader checks (--shader-checks): one thread fails a check on purpose, and the host expects exactly
// one failure with the values 12345 and 678 in the report -- proof that the report texture, its atomics through the descriptor heap and
// the copy back to the CPU work on this GPU and API before the checks are trusted to stay silent.
#include "nox_gpu.slang"

[shader("compute")]
[numthreads(1, 1, 1)]
void csNoxCheckSelfTest()
{
    NOX_CHECK(false, 12345, 678);
}
