#pragma once

#include <qdb/exec/executor.h>
#include <qdb/kernel/compiler.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace NQqb {

// One output column: either a zero-copy reference to an input column (ident
// projection) or a computed column filled by the project kernel.
struct TProjectColumn {
    bool Computed = false;
    // Ident: index into the input batch's columns.
    // Computed: index into the computed-buffer list (and computedTypes).
    int32_t Index = 0;
};

// Projects input rows. Ident columns are zero-copy (the output TColumn points
// into the retained input batch); only computed columns are materialized via
// the CompileProject kernel into owned buffers.
class TRuntimeProject : public IRuntimeNode {
public:
    TRuntimeProject(
        std::unique_ptr<IRuntimeNode> input,
        NQumir::NAst::TTypePtr outputType,
        std::vector<TProjectColumn> columns,
        TKernelCompiler::TProjectDispatch computeDispatch,
        std::vector<size_t> computedWidths);

    NQumir::NAst::TTypePtr OutputType() const override { return OutputType_; }
    bool Next(TRowSet& rowSet) override;

private:
    std::unique_ptr<IRuntimeNode> Input_;
    NQumir::NAst::TTypePtr OutputType_;
    std::vector<TProjectColumn> Columns_;
    TKernelCompiler::TProjectDispatch ComputeDispatch_; // null if no computed columns
    std::vector<size_t> ComputedWidths_;                // byte width per computed column
};

} // namespace NQqb
