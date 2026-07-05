#pragma once

#include <qdb/exec/unary_process.h>
#include <qdb/kernel/compiler.h>

namespace NQdb {

TUnaryStreamProcess MakeFilterProcess(
    TKernelCompiler::TFilterDispatch dispatch);

} // namespace NQdb
