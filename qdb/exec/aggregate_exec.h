#pragma once

#include <qdb/exec/unary_block_exec.h>
#include <qdb/kernel/compiler.h>

namespace NQdb {

TRuntimeUnaryBlockingKernel::TProcess MakeAggregateProcess(
    TAggregateKernels kernels);

} // namespace NQdb
