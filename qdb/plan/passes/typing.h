#pragma once

#include <qdb/plan/ops/operator.h>

namespace NQqb {

// Bottom-up pass: assigns TFunctionType([required], output) to every operator.
//   Source : fun()          -> struct(all columns from schema)
//   Filter : fun(input_schema) -> input_schema
//   Project: fun(input_schema) -> struct(projection specs)
//
// ParamTypes[0] = full input schema initially; narrowed by ApplyColumnPushdown.
// Idempotent — safe to call again after plan modifications.
void AnnotateTypes(const TOperatorPtr& root);

} // namespace NQqb
