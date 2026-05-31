#include <qdb/ops/used_columns.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/project.h>
#include <qdb/ops/source.h>

#include <qumir/parser/ast.h>

#include <functional>

namespace NQqb {

namespace {

// TODO: brittle — conflates kernel variable names with column names.
// Proper fix: store ReferencedColumns() explicitly on IOperator.
void WalkIdents(
    const NQumir::NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& fieldNames,
    std::unordered_set<std::string>& out)
{
    if (!expr) {
        return;
    }
    if (auto node = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(expr)) {
        const auto& name = node.Cast()->Name;
        if (fieldNames.count(name)) {
            out.insert(name);
        }
    }
    for (const auto& child : expr->Children()) {
        WalkIdents(child, fieldNames, out);
    }
}

std::unordered_set<std::string> FieldsInExpr(
    const NQumir::NAst::TExprPtr& expr,
    const NQumir::NAst::TStructType& inputType)
{
    std::unordered_set<std::string> fieldNames;
    for (const auto& [name, _] : inputType.Fields) {
        fieldNames.insert(name);
    }
    std::unordered_set<std::string> result;
    WalkIdents(expr, fieldNames, result);
    return result;
}

} // namespace

std::unordered_set<std::string> CollectUsedColumns(const TOperatorPtr& op) {
    if (TMaybeOp<TSourceOperator>(op)) {
        return {};
    }

    if (auto maybe = TMaybeOp<TFilterOperator>(op)) {
        auto filter = maybe.Cast();
        auto cols = CollectUsedColumns(filter->Input());
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(filter->Input()->Type.get());
        if (inputType) {
            for (auto& c : FieldsInExpr(filter->Predicate(), *inputType)) {
                cols.insert(c);
            }
        }
        return cols;
    }

    if (auto maybe = TMaybeOp<TProjectOperator>(op)) {
        auto project = maybe.Cast();
        auto cols = CollectUsedColumns(project->Input());
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(project->Input()->Type.get());
        if (inputType) {
            for (const auto& proj : project->Projections()) {
                for (auto& c : FieldsInExpr(proj.Expression, *inputType)) {
                    cols.insert(c);
                }
            }
        }
        return cols;
    }

    return {};
}

} // namespace NQqb
