#pragma once

#include <qdb/ops/operator.h>

#include <qumir/error.h>

#include <expected>
#include <string>
#include <vector>

namespace NQqb {

struct TProjectionSpec {
    std::string Name;
    NQumir::NAst::TExprPtr Expression; // parsed, unannotated
};

class TProjectOperator : public IOperator {
public:
    static constexpr const char* OpId = "project";

    TProjectOperator(TOperatorPtr input, std::vector<TProjectionSpec> projections);

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override;
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Input_}; }
    const std::string ToString() const override;

    TOperatorPtr Input() const { return Input_; }
    const std::vector<TProjectionSpec>& Projections() const { return Projections_; }

private:
    TOperatorPtr Input_;
    std::vector<TProjectionSpec> Projections_;
};

// projections: list of (output_name, expression_string) pairs
std::expected<TOperatorPtr, NQumir::TError>
MakeProject(TOperatorPtr input, std::vector<std::pair<std::string, std::string>> projections);

} // namespace NQqb
