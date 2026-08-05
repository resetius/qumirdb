#pragma once

#include <qdb/exec/unary_process.h>
#include <qdb/kernel/compiler.h>

#include <cstdint>
#include <vector>

namespace NQdb {

TUnaryStreamProcess MakeFilterProcess(
    TKernelCompiler::TFilterDispatch dispatch);

// Filter that also drops input columns absent from the output schema: applies
// the predicate (which still sees every input column) then emits only the kept
// columns (zero-copy). keptIndices are input column indices, in output order.
TUnaryStreamProcess MakeFilterSelectProcess(
    TKernelCompiler::TFilterDispatch dispatch,
    std::vector<int32_t> keptIndices);

} // namespace NQdb
