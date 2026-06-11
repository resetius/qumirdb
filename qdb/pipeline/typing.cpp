#include <qdb/pipeline/typing.h>

#include <qdb/ops/aggregate.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/project.h>
#include <qdb/ops/source.h>

#include <qumir/parser/type.h>

namespace NQqb {

using namespace NQumir::NAst;

void AnnotateTypes(const TOperatorPtr& root) {
    // Bottom-up: children first.
    for (const auto& child : root->Children()) {
        if (auto maybeOp = TMaybeNode<IOperator>(child)) {
            AnnotateTypes(maybeOp.Cast());
        }
    }

    if (auto maybe = TMaybeOp<TSourceOperator>(root)) {
        root->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{},
            StructTypeFromSchema(maybe.Cast()->GetSource().Schema()));
        return;
    }

    if (auto maybe = TMaybeOp<TFilterOperator>(root)) {
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
}

} // namespace NQqb
