#pragma once

#include <qdb/kernel/generated.h>

#include <iosfwd>
#include <span>

namespace NQdb {

void JitFinalizeKernelsFused(
    std::span<TGeneratedKernel> kernels,
    std::ostream* diagnostics);

} // namespace NQdb
