#pragma once

#include <qdb/exec/join_exec.h>
#include <qdb/exec/project_exec.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/exec/unary_process.h>
#include <qdb/kernel/compiler.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/window.h>

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
    std::vector<bool> ComputedIsNullable;
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

// Build-side selection for an inner join. Shared by native lowering and the
// browser plan export so the JoinAsymmetryRatio threshold has one home (C++).
EJoinBuildSide ChooseJoinBuildSide(const TJoinOperator& join);
std::string_view JoinBuildSideName(EJoinBuildSide side);

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

struct TWindowRuntimeProcess {
    NQumir::NAst::TTypePtr OutputType;
    std::vector<TSortKey> Keys;              // partition (asc) ++ order
    std::vector<TSortColumnRef> KeyColumns;
    TSortRadixKernel Kernel;                 // Kernel.Dispatch = window kernel
};

TWindowRuntimeProcess BuildWindowRuntimeProcess(
    TWindowOperator& window,
    const NQumir::NAst::TTypePtr& inputType,
    TKernelCompilerOptions options);

} // namespace NQdb
