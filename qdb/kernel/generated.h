#pragma once

#include <qumir/parser/ast.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NQumir {
class TLLVMRunner;
} // namespace NQumir

namespace NQdb {

// Machine binding for one generated kernel program. Created empty at
// generation time; filled exactly once by the JIT finalizer.
struct TKernelSlot {
    // Parallel to TGeneratedKernel::Entrypoints; nullptr until bound.
    std::vector<void*> Fns;
    // Owns the JIT'd code: the pointers are valid while this lives.
    std::shared_ptr<NQumir::TLLVMRunner> Runner;
};

// One kernel program attached to the physical plan as an AST. A finalizer
// then either JITs it (fills Slot) or compiles it to wasm.
struct TGeneratedKernel {
    std::string Name;                       // "filter", "join", "aggregate.update", ...
    std::string Stage;                      // == TTaskNode::DebugGroup of the owning stage
    std::vector<std::string> Entrypoints;
    NQumir::NAst::TExprPtr Ast;
    // Payloads the AST points into (e.g. filter literal strings).
    std::shared_ptr<void> Storage;
    // Logical-operator identity; joins exec-plan stages to their kernels.
    const void* Operator = nullptr;
    std::shared_ptr<TKernelSlot> Slot;

    // Sort kernels only: resolved key metadata for the exec exporter.
    struct TSortKeyMeta {
        int32_t Index = 0;
        int32_t WidthBytes = 0;
        bool Desc = false;
    };
    std::vector<TSortKeyMeta> SortKeys;
};

} // namespace NQdb
