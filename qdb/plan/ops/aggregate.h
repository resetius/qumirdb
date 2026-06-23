#pragma once

#include <qdb/plan/ops/operator.h>

#include <qumir/error.h>

#include <expected>
#include <string>
#include <tuple>
#include <vector>

namespace NQdb {

struct TAggregateSpec {
    std::string Name;            // output column name
    std::string Func;            // "count" | "sum" | "min" | "max"
    NQumir::NAst::TExprPtr Arg;  // parsed, unannotated; nullptr for count(*)
};

class TAggregateOperator : public IOperator {
public:
    static constexpr const char* OpId = "aggregate";

    TAggregateOperator(TOperatorPtr input, std::vector<std::string> groupKeys, std::vector<TAggregateSpec> aggs);

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override;
    // Aggregate defines a new schema: child only needs group keys + agg args.
    std::unordered_set<std::string> RequiredColumnsForChild(
        size_t, const std::unordered_set<std::string>&) const override {
        return ComputeReferencedColumns();
    }
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Input_}; }
    const std::string ToString() const override;

    TOperatorPtr Input() const { return Input_; }
    const std::vector<std::string>& GroupKeys() const { return GroupKeys_; }
    std::vector<std::string>& MutableGroupKeys() { return GroupKeys_; }
    const std::vector<TAggregateSpec>& Aggs() const { return Aggs_; }
    std::vector<TAggregateSpec>& MutableAggs() { return Aggs_; }

private:
    TOperatorPtr Input_;
    std::vector<std::string> GroupKeys_;
    std::vector<TAggregateSpec> Aggs_;
};

// Output schema = group key columns (types from inputSchema) followed by one
// column per aggregate (type derived from Func/Arg). Used both by the
// constructor and by AnnotateTypes (after column pruning narrows inputSchema).
NQumir::NAst::TTypePtr ComputeAggregateOutputType(
    const NQumir::NAst::TTypePtr& inputSchema,
    const std::vector<std::string>& groupKeys,
    const std::vector<TAggregateSpec>& aggs);

// aggs: (output_name, func, arg_expr_string) triples; arg_expr_string empty for count(*)
std::expected<TOperatorPtr, NQumir::TError>
MakeAggregate(TOperatorPtr input, std::vector<std::string> groupKeys,
    std::vector<std::tuple<std::string, std::string, std::string>> aggs);

} // namespace NQdb
