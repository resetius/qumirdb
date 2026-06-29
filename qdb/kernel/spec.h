#pragma once

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/sort.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace NQdb {
namespace NKernel {

enum class EOperatorKernelKind {
    NonCompute,
    UnaryStreaming,
    UnaryBlocking,
    Binary,
};

struct TKernelColumnRef {
    std::string Name;
    int32_t Index = -1;
    NQumir::NAst::TTypePtr Type;
};

struct TKernelKeySpec {
    std::string Name;
    std::vector<TKernelColumnRef> Columns;
};

struct TKernelEntrypointSpec {
    std::string Name;
    std::string Abi;
};

struct TKernelAggregateSpec {
    std::string Name;
    std::string Func;
    bool HasArg = false;
    TKernelColumnRef Arg;
    NQumir::NAst::TExprPtr ArgExpr;
};

struct TKernelSortKeySpec {
    TKernelColumnRef Column;
    ESortDirection Direction = ESortDirection::Asc;
    ESortNulls Nulls = ESortNulls::Default;
};

struct TOperatorKernelSpec {
    EOperatorKernelKind Kind = EOperatorKernelKind::NonCompute;
    std::string OperatorName;
    std::vector<NQumir::NAst::TTypePtr> InputSchemas;
    NQumir::NAst::TTypePtr OutputSchema;
    std::vector<TKernelColumnRef> ReferencedColumns;
    std::vector<NQumir::NAst::TExprPtr> Expressions;
    std::vector<TKernelKeySpec> Keys;
    std::vector<TKernelAggregateSpec> Aggregates;
    std::vector<TKernelSortKeySpec> SortKeys;
    std::vector<TKernelEntrypointSpec> Entrypoints;
    std::vector<std::string> SourceModules;
};

std::string_view KernelKindName(EOperatorKernelKind kind);

TOperatorKernelSpec BuildFilterKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const NQumir::NAst::TExprPtr& predicate,
    std::string entrypointName = "<kernel>");

TOperatorKernelSpec BuildProjectKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<NQumir::NAst::TExprPtr>& computedExprs,
    const std::vector<NQumir::NAst::TTypePtr>& computedTypes,
    std::string entrypointName = "<project>");

TOperatorKernelSpec BuildAggregateKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys,
    const std::vector<TAggregateSpec>& aggs);

TOperatorKernelSpec BuildSortKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<TSortKey>& keys,
    std::string operatorName = "sort");

void PrintKernelSpec(std::ostream& out, const TOperatorKernelSpec& spec);

} // namespace NKernel
} // namespace NQdb
