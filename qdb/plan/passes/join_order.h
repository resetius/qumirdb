#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Reorders inner-join chains (comma joins) into a connected left-deep order
// using the equalities of the governing WHERE filter, so that only genuinely
// disconnected relations stay cross-joined. Each relation is attached next to a
// relation it shares an equality with, so the subsequent equi-join extraction
// can lift a key for every connected adjacency.
//
// Runs after QualifyColumns and AnnotateTypes (it needs leaf output schemas to
// map columns to relations) and before ExtractEquiJoins. Returns the new root.
TOperatorPtr ReorderJoins(TOperatorPtr root);

} // namespace NQdb
