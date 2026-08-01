#pragma once

#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/operator.h>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace NQdb {

enum class ECteReuseMode {
    Inline,
    Materialize,
};

struct TCteReuseDecision {
    ECteReuseMode Mode;
    std::unordered_set<std::string> RequiredOutputs; // for the 5b boundary projection
};
using TCteReuseDecisions = std::unordered_map<TCteDefinition*, TCteReuseDecision>;

// >=2 references -> Materialize, else Inline.
TCteReuseDecisions ChooseCteReuse(const TCteUsageMap& usage);

// Realizes each TCteRef per its decision: Inline splices a private clone,
// Materialize leaves a TCteConsumer sharing one TCteMaterialization.
TOperatorPtr ApplyCteReuse(TOperatorPtr plan, const TCteReuseDecisions& decisions);

} // namespace NQdb
