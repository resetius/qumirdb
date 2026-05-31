#pragma once

#include <qdb/io/io.h>

#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace NQqb {

// Compiles qumir core-lang kernel sources to LLVM JIT function pointers.
class TKernelCompiler {
public:
    // Filter dispatch: called per batch to fill the selection buffer.
    using TFilterDispatch = std::function<void(const TRowSet& rowSet, bool* selection)>;

    // Compiles a filter kernel for the given input struct type and predicate.
    // Returns a dispatch lambda that calls the compiled kernel.
    TFilterDispatch CompileFilter(
        const NQumir::NAst::TStructType& inputType,
        const NQumir::NAst::TExprPtr& predicate);

};

} // namespace NQqb
