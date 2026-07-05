#pragma once

#include <qdb/exec/unary_process.h>
#include <qdb/kernel/compiler.h>

#include <cstdint>
#include <vector>

namespace NQdb {

// One output column: either a zero-copy reference to an input column (ident
// projection) or a computed column filled by the project kernel.
struct TProjectColumn {
    bool Computed = false;
    // Ident: index into the input batch's columns.
    // Computed: index into the computed-buffer list (and computedTypes).
    int32_t Index = 0;
};

TUnaryStreamProcess MakeProjectProcess(
    std::vector<TProjectColumn> columns,
    TKernelCompiler::TProjectDispatch computeDispatch,
    std::vector<size_t> computedWidths,
    std::vector<bool> computedIsString = {});

} // namespace NQdb
