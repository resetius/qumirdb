#pragma once

#include <qdb/kernel/generated.h>

#include <iosfwd>
#include <span>

namespace NQdb {

// Compile each kernel's AST with the native JIT and fill its slot (Fns per
// entrypoint + owning runner). Idempotent — already-bound slots are skipped.
// Throws on compilation failure.
void JitFinalizeKernels(
    std::span<TGeneratedKernel> kernels,
    std::ostream* diagnostics = nullptr);

} // namespace NQdb
