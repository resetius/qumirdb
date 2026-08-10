#pragma once

#include <qdb/plan/ops/operator.h>

#include <qumir/error.h>

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

namespace NQdb {

struct TProjectionSpec {
    std::string Name;
    NQumir::NAst::TExprPtr Expression; // parsed, unannotated
    // Explicit names from `AS (x, y, z)`. Empty for scalar aliases and for the
    // default colN naming of a flattened struct projection.
    std::vector<std::string> ColumnAliases;
    // True for an unnamed SQL expression (`SELECT a + b`). Type annotation may
    // shift its synthetic colN name when an earlier projection expands a struct.
    // For a struct expression, Name anchors the group at its first flattened column.
    bool ImplicitName = false;
    // SELECT DISTINCT is built before struct arity is known. Reject an unaliased
    // struct during typing instead of silently deduplicating only its first field.
    bool RejectUnaliasedStructForDistinct = false;
};

// Number of output columns occupied after flattening a struct-valued projection.
size_t FlattenedProjectionArity(const TProjectionSpec& projection);

class TProjectOperator : public IOperator {
public:
    static constexpr const char* OpId = "project";

    TProjectOperator(TOperatorPtr input, std::vector<TProjectionSpec> projections);

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override;
    // Project defines a new schema: child only needs project's own refs.
    std::unordered_set<std::string> RequiredColumnsForChild(
        size_t, const std::unordered_set<std::string>&) const override {
        return ComputeReferencedColumns();
    }
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Input_}; }
    const std::string ToString() const override;

    TOperatorPtr Input() const { return Input_; }
    TOperatorPtr& MutableInput() { return Input_; }
    const std::vector<TProjectionSpec>& Projections() const { return Projections_; }
    std::vector<TProjectionSpec>& MutableProjections() { return Projections_; }

private:
    TOperatorPtr Input_;
    std::vector<TProjectionSpec> Projections_;
};

// projections: list of (output_name, expression_string) pairs
std::expected<TOperatorPtr, NQumir::TError>
MakeProject(TOperatorPtr input, std::vector<std::pair<std::string, std::string>> projections);

} // namespace NQdb
