#pragma once

#include <qdb/ops/operator.h>

#include <qumir/error.h>

#include <expected>
#include <string>
#include <vector>

namespace NQqb {

struct TProjectionSpec {
    std::string Name;
    std::string Expression; // core-lang expression
};

// Project operator — logical plan node.
// Computes new columns; output type is a new TStructType derived from projections.
//
// Kernels_ holds one annotated TFunDecl per projection expression.
// Physical planner compiles them (or fuses into one kernel).
class TProjectOperator : public IOperator {
public:
    static constexpr const char* OpId = "project";

    TProjectOperator(TOperatorPtr input, std::vector<TProjectionSpec> projections, std::vector<NQumir::NAst::TExprPtr> kernels);

    std::string_view RelName() const override { return OpId; }

    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Input_}; }

    const std::string ToString() const override;

    TOperatorPtr Input() const { return Input_; }
    const std::vector<TProjectionSpec>& Projections() const { return Projections_; }
    const std::vector<NQumir::NAst::TExprPtr>& Kernels() const { return Kernels_; }

private:
    TOperatorPtr Input_;
    std::vector<TProjectionSpec> Projections_;
    std::vector<NQumir::NAst::TExprPtr> Kernels_; // one annotated TFunDecl per output column
};

// Parses and annotates each expression; infers output TStructType; builds TProjectOperator.
std::expected<TOperatorPtr, NQumir::TError>
MakeProject(TOperatorPtr input, std::vector<TProjectionSpec> projections);

} // namespace NQqb
