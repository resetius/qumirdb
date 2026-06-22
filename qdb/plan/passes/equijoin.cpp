#include <qdb/plan/passes/equijoin.h>

namespace NQqb {

TOperatorPtr ExtractEquiJoins(TOperatorPtr root) {
    // TODO: lift equi-predicates into join keys (see PLAN_EQUIJOIN.md).
    return root;
}

} // namespace NQqb
