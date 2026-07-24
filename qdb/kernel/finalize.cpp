#include <qdb/kernel/finalize.h>

#include <qdb/kernel/finalize_fused.h>

namespace NQdb {

void JitFinalizeKernels(
    std::span<TGeneratedKernel> kernels,
    std::ostream* diagnostics)
{
    JitFinalizeKernelsFused(kernels, diagnostics);
}

} // namespace NQdb
