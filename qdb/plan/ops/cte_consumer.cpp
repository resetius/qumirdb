#include <qdb/plan/ops/cte_consumer.h>

#include <qumir/parser/type.h>

#include <unordered_map>
#include <unordered_set>

namespace NQdb {

TCteConsumer::TCteConsumer(TCteDefinitionPtr def, TCteMaterializationPtr materialization)
    : Def_(std::move(def))
    , Materialization_(std::move(materialization))
{
    Type = std::make_shared<NQumir::NAst::TFunctionType>(
        std::vector<NQumir::NAst::TTypePtr>{}, Materialization_->Plan->OutputColumns());
}

const std::string TCteConsumer::ToString() const {
    return "(rel cte-consumer)";
}

namespace {

void CountConsumers(
    const TOperatorPtr& op,
    std::unordered_set<TCteMaterialization*>& seen,
    std::unordered_map<TCteMaterialization*, size_t>& counts) {
    if (!op) {
        return;
    }
    if (auto consumer = TMaybeOp<TCteConsumer>(op)) {
        auto* mat = consumer.Cast()->Materialization().get();
        ++counts[mat];
        if (seen.insert(mat).second) {
            CountConsumers(mat->Plan, seen, counts);
        }
        return;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            CountConsumers(childOp.Cast(), seen, counts);
        }
    }
}

} // namespace

void AssignMaterializationRefCounts(const TOperatorPtr& plan) {
    std::unordered_set<TCteMaterialization*> seen;
    std::unordered_map<TCteMaterialization*, size_t> counts;
    CountConsumers(plan, seen, counts);
    for (const auto& [mat, count] : counts) {
        mat->RefCount = count;
    }
}

namespace {

void CollectMats(
    const TOperatorPtr& op,
    std::unordered_set<TCteMaterialization*>& seen,
    std::vector<TMaterializationRef>& out) {
    if (!op) {
        return;
    }
    if (auto consumer = TMaybeOp<TCteConsumer>(op)) {
        const auto& mat = consumer.Cast()->Materialization();
        if (seen.insert(mat.get()).second) {
            CollectMats(mat->Plan, seen, out);
            out.push_back(TMaterializationRef{consumer.Cast()->Def()->Id, mat});
        }
        return;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            CollectMats(childOp.Cast(), seen, out);
        }
    }
}

} // namespace

std::vector<TMaterializationRef> CollectMaterializations(const TOperatorPtr& plan) {
    std::unordered_set<TCteMaterialization*> seen;
    std::vector<TMaterializationRef> out;
    CollectMats(plan, seen, out);
    return out;
}

} // namespace NQdb
