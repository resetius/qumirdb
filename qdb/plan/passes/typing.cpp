#include <qdb/plan/passes/typing.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/kernel/project_type.h>

#include <qumir/parser/type.h>

namespace NQdb {

using namespace NQumir::NAst;

void AnnotateTypes(const TOperatorPtr& root) {
    // Bottom-up: children first.
    for (const auto& child : root->Children()) {
        if (auto maybeOp = TMaybeNode<IOperator>(child)) {
            AnnotateTypes(maybeOp.Cast());
        }
    }

    if (auto maybe = TMaybeOp<TSourceOperator>(root)) {
        auto src = maybe.Cast();
        // If QualifyColumns has already set a qualified schema, keep it.
        if (!src->GetAlias().empty()) return;
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{},
            StructTypeFromSchema(src->GetSource().Schema()));
        return;
    }

    if (auto maybe = TMaybeOp<TFilterOperator>(root)) {
        auto schema = maybe.Cast()->Input()->OutputColumns();
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{schema},
            schema);
        return;
    }

    if (auto maybe = TMaybeOp<TSortOperator>(root)) {
        auto schema = maybe.Cast()->Input()->OutputColumns();
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{schema},
            schema);
        return;
    }

    if (auto maybe = TMaybeOp<TTopSortOperator>(root)) {
        auto schema = maybe.Cast()->Input()->OutputColumns();
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{schema},
            schema);
        return;
    }

    if (auto maybe = TMaybeOp<TLimitOperator>(root)) {
        auto schema = maybe.Cast()->Input()->OutputColumns();
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{schema},
            schema);
        return;
    }

    if (auto maybe = TMaybeOp<TProjectOperator>(root)) {
        auto proj = maybe.Cast();
        auto* inputStruct = static_cast<TStructType*>(proj->Input()->OutputColumns().get());
        std::vector<std::pair<std::string, TTypePtr>> outFields;
        if (inputStruct) {
            for (const auto& spec : proj->Projections()) {
                TTypePtr fieldType;
                if (auto ident = TMaybeNode<TIdentExpr>(spec.Expression)) {
                    for (auto& [name, type] : inputStruct->Fields) {
                        if (name == ident.Cast()->Name) { fieldType = type; break; }
                    }
                } else {
                    // Computed column: infer the expression's result type.
                    fieldType = NKernel::InferProjectExprType(spec.Expression, *inputStruct);
                }
                outFields.emplace_back(spec.Name, fieldType);
            }
        }
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{proj->Input()->OutputColumns()},
            std::make_shared<TStructType>(std::move(outFields)));
        return;
    }

    if (auto maybe = TMaybeOp<TAggregateOperator>(root)) {
        auto agg = maybe.Cast();
        auto inputSchema = agg->Input()->OutputColumns();
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{inputSchema},
            ComputeAggregateOutputType(inputSchema, agg->GroupKeys(), agg->Aggs()));
        return;
    }

    if (auto maybe = TMaybeOp<TJoinOperator>(root)) {
        auto join = maybe.Cast();
        auto leftSchema = join->Left()->OutputColumns();
        auto rightSchema = join->Right()->OutputColumns();
        auto output = ComputeJoinOutputType(leftSchema, rightSchema, join->JoinType());
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{leftSchema, rightSchema},
            output ? *output : nullptr);
        return;
    }
}

} // namespace NQdb
