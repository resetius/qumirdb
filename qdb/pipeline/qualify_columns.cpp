#include <qdb/pipeline/qualify_columns.h>

#include <qdb/ops/aggregate.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/join.h>
#include <qdb/ops/project.h>
#include <qdb/ops/source.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <vector>

namespace NQqb {

using namespace NQumir::NAst;

namespace {

// Extract a lowercase stem from a path like "/data/nation.parquet" or "__NATION__".
std::string ExtractStem(const std::string& path) {
    auto lastSlash = path.rfind('/');
    std::string name = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
    auto dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    auto start = name.find_first_not_of('_');
    auto end   = name.find_last_not_of('_');
    if (start == std::string::npos) return name;
    name = name.substr(start, end - start + 1);
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c){ return std::tolower(c); });
    return name;
}

// Collect all TSourceOperator nodes in pre-order.
void CollectSources(const TOperatorPtr& op,
    std::vector<std::shared_ptr<TSourceOperator>>& out)
{
    if (auto maybe = TMaybeOp<TSourceOperator>(op)) {
        out.push_back(maybe.Cast());
        return;
    }
    for (auto& child : op->Children()) {
        if (auto maybeOp = TMaybeNode<IOperator>(child)) {
            CollectSources(maybeOp.Cast(), out);
        }
    }
}

// Look up `name` in `schema`. If it already contains '.' return as-is.
// Matches by the bare part after the last '.' in each field name.
// Returns the qualified field name, or `name` unchanged if no match found.
std::string QualifyIdent(const TStructType& schema, const std::string& name) {
    if (name.find('.') != std::string::npos) return name; // already qualified

    std::string_view sv = name;
    std::string result;
    int matches = 0;
    for (const auto& [fieldName, _] : schema.Fields) {
        auto dot = fieldName.rfind('.');
        std::string_view bare = (dot != std::string::npos)
            ? std::string_view(fieldName).substr(dot + 1)
            : std::string_view(fieldName);
        if (bare == sv) {
            result = fieldName;
            ++matches;
        }
    }
    if (matches == 1) return result;
    return name; // zero or ambiguous → keep original (best-effort)
}

// Recursively rename TIdentExpr leaves in an expression tree.
void QualifyExpr(TExprPtr& expr, const TStructType& schema) {
    if (!expr) return;
    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        ident.Cast()->Name = QualifyIdent(schema, ident.Cast()->Name);
        return;
    }
    for (TExprPtr* child : expr->MutableChildren()) {
        QualifyExpr(*child, schema);
    }
}

// Bottom-up pass: returns the qualified output schema for each node.
// Also modifies the node in-place (source Type, join keys, expression idents, etc.)
std::shared_ptr<TStructType> QualifyColumnsImpl(const TOperatorPtr& op) {

    // ── Source ──────────────────────────────────────────────────────────────
    if (auto maybe = TMaybeOp<TSourceOperator>(op)) {
        auto src = maybe.Cast();
        const std::string& alias = src->GetAlias();
        const auto& schema = src->GetSource().Schema();

        std::vector<std::pair<std::string, TTypePtr>> newFields;
        newFields.reserve(schema.Columns.size());
        for (const auto& col : schema.Columns) {
            newFields.emplace_back(alias + "." + std::string(col.Name), col.Type);
        }
        auto qStruct = std::make_shared<TStructType>(std::move(newFields));

        // Update the source's TFunctionType::ReturnType in-place.
        if (auto* fun = static_cast<TFunctionType*>(src->Type.get())) {
            fun->ReturnType = qStruct;
        } else {
            src->Type = std::make_shared<TFunctionType>(
                std::vector<TTypePtr>{}, qStruct);
        }
        return qStruct;
    }

    // ── Filter ──────────────────────────────────────────────────────────────
    if (auto maybe = TMaybeOp<TFilterOperator>(op)) {
        auto flt = maybe.Cast();
        auto childSchema = QualifyColumnsImpl(flt->Input());
        if (childSchema) {
            QualifyExpr(flt->MutablePredicate(), *childSchema);
        }
        return childSchema;
    }

    // ── Join ────────────────────────────────────────────────────────────────
    if (auto maybe = TMaybeOp<TJoinOperator>(op)) {
        auto join = maybe.Cast();
        auto leftSchema  = QualifyColumnsImpl(join->Left());
        auto rightSchema = QualifyColumnsImpl(join->Right());

        // Update equi-join keys.
        if (leftSchema && rightSchema) {
            for (auto& key : join->MutableKeys()) {
                key.Left  = QualifyIdent(*leftSchema,  key.Left);
                key.Right = QualifyIdent(*rightSchema, key.Right);
            }
        }

        // Update optional residual predicate (uses merged schema).
        if (join->MutableFilter() && leftSchema && rightSchema) {
            std::vector<std::pair<std::string, TTypePtr>> merged = leftSchema->Fields;
            for (auto& f : rightSchema->Fields) merged.push_back(f);
            TStructType mergedStruct(std::move(merged));
            QualifyExpr(join->MutableFilter(), mergedStruct);
        }

        // Build the qualified output schema (concatenation, no duplicates after qualification).
        if (!leftSchema || !rightSchema) return nullptr;
        auto outFields = leftSchema->Fields;
        for (auto& f : rightSchema->Fields) outFields.push_back(f);
        auto outSchema = std::make_shared<TStructType>(std::move(outFields));

        // Update the join's TFunctionType::ReturnType so AnnotateTypes can skip it if needed.
        // (AnnotateTypes will re-compute it anyway, so this just ensures a non-null ReturnType
        //  is available during the QualifyColumns bottom-up walk for ancestor nodes.)
        if (auto* fun = static_cast<TFunctionType*>(join->Type.get())) {
            fun->ReturnType = outSchema;
        }
        return outSchema;
    }

    // ── Project ─────────────────────────────────────────────────────────────
    if (auto maybe = TMaybeOp<TProjectOperator>(op)) {
        auto proj = maybe.Cast();
        auto childSchema = QualifyColumnsImpl(proj->Input());
        if (childSchema) {
            for (auto& spec : proj->MutableProjections()) {
                QualifyExpr(spec.Expression, *childSchema);
            }
        }
        // Output schema is user-defined names; types will be filled by AnnotateTypes.
        std::vector<std::pair<std::string, TTypePtr>> outFields;
        for (const auto& spec : proj->Projections()) {
            outFields.emplace_back(spec.Name, nullptr);
        }
        return std::make_shared<TStructType>(std::move(outFields));
    }

    // ── Aggregate ───────────────────────────────────────────────────────────
    if (auto maybe = TMaybeOp<TAggregateOperator>(op)) {
        auto agg = maybe.Cast();
        auto childSchema = QualifyColumnsImpl(agg->Input());
        if (childSchema) {
            for (auto& key : agg->MutableGroupKeys()) {
                key = QualifyIdent(*childSchema, key);
            }
            for (auto& spec : agg->MutableAggs()) {
                QualifyExpr(spec.Arg, *childSchema);
            }
        }
        // Output schema is user-defined; types filled by AnnotateTypes.
        std::vector<std::pair<std::string, TTypePtr>> outFields;
        for (const auto& key : agg->GroupKeys()) {
            outFields.emplace_back(key, nullptr);
        }
        for (const auto& spec : agg->Aggs()) {
            outFields.emplace_back(spec.Name, nullptr);
        }
        return std::make_shared<TStructType>(std::move(outFields));
    }

    // Unknown operator — recurse children to qualify sources beneath it.
    for (auto& child : op->Children()) {
        if (auto maybeOp = TMaybeNode<IOperator>(child)) {
            QualifyColumnsImpl(maybeOp.Cast());
        }
    }
    return nullptr;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────

void AssignSourceAliases(const TOperatorPtr& root) {
    std::vector<std::shared_ptr<TSourceOperator>> sources;
    CollectSources(root, sources);

    // Count stems (only for sources without an explicit alias).
    std::unordered_map<std::string, int> stemCount;
    for (auto& src : sources) {
        if (src->GetAlias().empty()) {
            stemCount[ExtractStem(src->SourcePath())]++;
        }
    }

    // Assign aliases: unique stem → bare stem, duplicate stem → stem_0, stem_1, …
    std::unordered_map<std::string, int> stemIndex;
    for (auto& src : sources) {
        if (!src->GetAlias().empty()) continue; // explicit alias already set
        std::string stem = ExtractStem(src->SourcePath());
        if (stemCount[stem] == 1) {
            src->SetAlias(stem);
        } else {
            int idx = stemIndex[stem]++;
            src->SetAlias(stem + "_" + std::to_string(idx));
        }
    }
}

void QualifyColumns(const TOperatorPtr& root) {
    QualifyColumnsImpl(root);
}

} // namespace NQqb
