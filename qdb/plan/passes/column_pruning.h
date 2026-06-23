#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQdb {

// Top-down pass: narrows TFunctionType::ParamTypes[0] of each operator to only
// the columns it actually needs, then calls SetRestrictedColumns on source nodes
// to restrict physical column reads from parquet.
//
// Must be called after AnnotateTypes (operators must have TFunctionType set).
void ApplyColumnPruning(const TOperatorPtr& root);

} // namespace NQdb
