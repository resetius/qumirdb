#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Rewrites empty-key inner joins into equi-joins by lifting equalities from the
// surrounding WHERE/ON predicates into join keys (transitive via equivalence
// classes). Runs after QualifyColumns and AnnotateTypes. Returns the new root
// (the top filter may be consumed).
TOperatorPtr ExtractEquiJoins(TOperatorPtr root);

TOperatorPtr PushDownPredicates(TOperatorPtr root);

} // namespace NQdb
