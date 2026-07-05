#pragma once

#include <qdb/exec/sort_exec.h>
#include <qdb/exec/unary_process.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/sort.h>

#include <iosfwd>
#include <string_view>
#include <vector>

namespace NQdb {

struct TUnaryRuntimeProcess {
    TUnaryStreamProcess Process;
    NQumir::NAst::TTypePtr OutputType;
};

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
    std::ostream* diagnostics);

TUnaryRuntimeProcess BuildProjectRuntimeProcess(
    TProjectOperator& project,
    const NQumir::NAst::TTypePtr& inputType,
    std::ostream* diagnostics);

TSortRuntimeProcess BuildSortRuntimeProcess(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortKey>& keys,
    std::string_view kernelName,
    std::ostream* diagnostics);

} // namespace NQdb
