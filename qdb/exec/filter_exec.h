#pragma once

#include <qdb/exec/unary_stream_exec.h>
#include <qdb/kernel/compiler.h>

namespace NQdb {

TRuntimeUnaryStreamingKernel::TProcess MakeFilterProcess(
    TKernelCompiler::TFilterDispatch dispatch);

} // namespace NQdb
