#include <qdb/plan/ops/cte_ref.h>

#include <qumir/parser/type.h>

#include <unordered_set>

namespace NQdb {

TCteRef::TCteRef(TCteDefinitionPtr def)
    : Def_(std::move(def))
{
    Type = std::make_shared<NQumir::NAst::TFunctionType>(
        std::vector<NQumir::NAst::TTypePtr>{}, Def_->OutputType);
}

const std::string TCteRef::ToString() const {
    return "(rel cte-ref)";
}

namespace {

void Collect(const TOperatorPtr& op, std::unordered_set<TCteDefinition*>& seen,
             std::vector<TCteDefinitionPtr>& out) {
    if (!op) {
        return;
    }
    if (auto ref = TMaybeOp<TCteRef>(op)) {
        auto def = ref.Cast()->Def();
        if (seen.insert(def.get()).second) {
            Collect(def->Plan, seen, out);
            out.push_back(def);
        }
        return;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            Collect(childOp.Cast(), seen, out);
        }
    }
}

} // namespace

std::vector<TCteDefinitionPtr> CollectCteDefinitions(const TOperatorPtr& plan) {
    std::unordered_set<TCteDefinition*> seen;
    std::vector<TCteDefinitionPtr> out;
    Collect(plan, seen, out);
    return out;
}

void AssignCteIds(const TOperatorPtr& plan) {
    uint32_t id = 0;
    for (const auto& def : CollectCteDefinitions(plan)) {
        def->Id = id++;
    }
}

} // namespace NQdb
