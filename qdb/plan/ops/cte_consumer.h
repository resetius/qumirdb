#pragma once

#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/operator.h>

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>

namespace NQdb {

// Resolved producer plan shared by every consumer; RefCount sizes the completion
// fan-out at lowering.
struct TCteMaterialization {
    TOperatorPtr Plan;
    size_t RefCount = 0;
};
using TCteMaterializationPtr = std::shared_ptr<TCteMaterialization>;

class TCteConsumer : public IOperator {
public:
    static constexpr const char* OpId = "cte-consumer";

    TCteConsumer(TCteDefinitionPtr def, TCteMaterializationPtr materialization);

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override { return {}; }
    const TCteDefinitionPtr& Def() const { return Def_; }
    const TCteMaterializationPtr& Materialization() const { return Materialization_; }
    const std::string ToString() const override;

private:
    TCteDefinitionPtr Def_;
    TCteMaterializationPtr Materialization_;
};

// Sets every reachable TCteMaterialization::RefCount to its physical consumer
// count (occurrences in the plan and in producer plans), which sizes the
// completion fan-out at lowering.
void AssignMaterializationRefCounts(const TOperatorPtr& plan);

} // namespace NQdb
