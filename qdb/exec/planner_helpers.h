#pragma once

#include <qdb/exec/project_exec.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/exec/unary_process.h>
#include <qdb/kernel/compiler.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/sort.h>

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace NQdb {

struct TUnaryRuntimeProcess {
    TUnaryStreamProcess Process;
    NQumir::NAst::TTypePtr OutputType;
};

// Compilation-free projection layout: column plan, computed-expression types
// and the output struct. Shared by the runtime process builder and the plan
// exporter so output typing has one source.
struct TProjectColumnPlan {
    std::vector<TProjectColumn> Columns;
    std::vector<NQumir::NAst::TExprPtr> ComputedExprs;
    std::vector<NQumir::NAst::TTypePtr> ComputedJitTypes;
    std::vector<size_t> ComputedWidths;
    std::vector<bool> ComputedIsString;
    NQumir::NAst::TTypePtr OutputType;
};

TProjectColumnPlan BuildProjectColumnPlan(
    TProjectOperator& project,
    const NQumir::NAst::TStructType& inputStruct);

struct TSortRuntimeProcess {
    std::vector<TSortColumnRef> KeyColumns;
    TSortRadixKernel RadixKernel;
};

// Print a human-readable runtime-plan tree for diagnostics (--verbose).
void PrintRuntimePlan(std::ostream& out, const TOperatorPtr& root);

NQumir::NAst::TTypePtr BuildSourceRuntimeType(TSourceOperator& src);

TUnaryRuntimeProcess BuildFilterRuntimeProcess(
    TFilterOperator& filter,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options);

TUnaryRuntimeProcess BuildProjectRuntimeProcess(
    TProjectOperator& project,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options);

TSortRuntimeProcess BuildSortRuntimeProcess(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortKey>& keys,
    std::string_view kernelName,
    TKernelCompilerOptions options);

} // namespace NQdb
