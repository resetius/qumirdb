#include <qdb/plan/plan_print.h>

#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>

#include <qumir/parser/core/printer.h>

#include <ostream>

namespace NQdb {

std::string ExprLine(const NQumir::NAst::TExprPtr& expr) {
    if (!expr) {
        return "";
    }
    return NQumir::NAst::NCore::PrintAst(
        expr,
        NQumir::NAst::NCore::TPrintOptions{.Pretty = false});
}

std::string JoinPlanLabel(const TJoinOperator& join) {
    std::string label = "join " + std::string(JoinTypeName(join.JoinType()));
    const auto& keys = join.Keys();
    for (size_t i = 0; i < keys.size(); ++i) {
        label += (i ? ", " : " [") + keys[i].Left + " = " + keys[i].Right;
    }
    if (!keys.empty()) {
        label += "]";
    }
    if (join.Filter()) {
        label += " residual " + ExprLine(join.Filter());
    }
    return label;
}

std::string AggregatePlanLabel(const TAggregateOperator& aggregate) {
    std::string label = "aggregate";
    const auto& keys = aggregate.GroupKeys();
    for (size_t i = 0; i < keys.size(); ++i) {
        label += (i ? ", " : " keys=[") + keys[i];
    }
    if (!keys.empty()) {
        label += "]";
    }
    const auto& aggs = aggregate.Aggs();
    for (size_t i = 0; i < aggs.size(); ++i) {
        label += (i ? ", " : " aggs=[") + aggs[i].Name + "=" + aggs[i].Func;
    }
    if (!aggs.empty()) {
        label += "]";
    }
    return label;
}

namespace {

std::string SortKeyLabel(const TSortKey& key) {
    std::string label = key.Column;
    label += " " + std::string(SortDirectionName(key.Direction));
    if (key.Nulls != ESortNulls::Default) {
        label += " " + std::string(SortNullsName(key.Nulls));
    }
    return label;
}

} // namespace

std::string SortPlanLabel(
    std::string_view kind,
    const std::vector<TSortKey>& keys,
    std::optional<int64_t> limit)
{
    std::string label(kind);
    for (size_t i = 0; i < keys.size(); ++i) {
        label += (i ? ", " : " [") + SortKeyLabel(keys[i]);
    }
    if (!keys.empty()) {
        label += "]";
    }
    if (limit) {
        label += " limit " + std::to_string(*limit);
    }
    return label;
}

std::string PlanLabel(const TOperatorPtr& op) {
    if (auto source = TMaybeOp<TSourceOperator>(op)) {
        auto src = source.Cast();
        std::string label = "source " + src->SourcePath();
        if (!src->GetAlias().empty()) {
            label += " AS " + src->GetAlias();
        }
        return label;
    }
    if (auto filter = TMaybeOp<TFilterOperator>(op)) {
        return "filter " + ExprLine(filter.Cast()->Predicate());
    }
    if (auto project = TMaybeOp<TProjectOperator>(op)) {
        std::string label = "project (";
        const auto& specs = project.Cast()->Projections();
        for (size_t i = 0; i < specs.size(); ++i) {
            label += (i ? ", " : "") + specs[i].Name;
        }
        return label + ")";
    }
    if (auto aggregate = TMaybeOp<TAggregateOperator>(op)) {
        return AggregatePlanLabel(*aggregate.Cast());
    }
    if (auto join = TMaybeOp<TJoinOperator>(op)) {
        return JoinPlanLabel(*join.Cast());
    }
    if (auto sort = TMaybeOp<TSortOperator>(op)) {
        return SortPlanLabel("sort", sort.Cast()->Keys());
    }
    if (auto sort = TMaybeOp<TTopSortOperator>(op)) {
        return SortPlanLabel("top-sort", sort.Cast()->Keys(), sort.Cast()->Limit());
    }
    return std::string(op->RelName());
}

std::vector<TOperatorPtr> ChildOps(const TOperatorPtr& op) {
    std::vector<TOperatorPtr> out;
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            out.push_back(childOp.Cast());
        }
    }
    return out;
}

void PrintPlanTree(
    std::ostream& out,
    const TOperatorPtr& op,
    const std::string& prefix,
    bool isLast,
    bool isRoot)
{
    out << prefix;
    if (!isRoot) {
        out << (isLast ? "└─ " : "├─ ");
    }
    out << PlanLabel(op);
    if (op->Stats_) {
        out << " (rows≈" << op->Stats_->RowCount << ")";
    }
    out << "\n";

    auto children = ChildOps(op);
    std::string childPrefix = isRoot ? prefix : prefix + (isLast ? "   " : "│  ");
    for (size_t i = 0; i < children.size(); ++i) {
        PrintPlanTree(out, children[i], childPrefix, i + 1 == children.size(), false);
    }
}

} // namespace NQdb
