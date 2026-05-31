#pragma once

#include <qdb/ops/operator.h>

#include <qumir/error.h>

#include <expected>
#include <string>

namespace NQqb {

// Filter operator — logical plan node.
// Sets TRowSet.Selection without materializing rows.
// Output type == input type (filter doesn't change schema).
//
// Kernel_ is the annotated TFunDecl of the predicate function,
// ready for the physical planner to compile.
class TFilterOperator : public IOperator {
public:
    static constexpr const char* OpId = "filter";

    TFilterOperator(TOperatorPtr input, std::string predicate, NQumir::NAst::TExprPtr kernel);

    std::string_view RelName() const override { return OpId; }

    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Input_}; }

    const std::string ToString() const override;

    const NQumir::NAst::TExprPtr& Kernel() const { return Kernel_; }
    const std::string& Predicate() const { return Predicate_; }
    TOperatorPtr Input() const { return Input_; }

private:
    TOperatorPtr Input_;
    std::string Predicate_;
    NQumir::NAst::TExprPtr Kernel_; // annotated TFunDecl of predicate
};

// Parses and annotates predicate; builds TFilterOperator.
// predicate: core-lang expression where struct fields are available as variables.
// Example: "(> o_totalprice (: 1000.0 f64))"
std::expected<TOperatorPtr, NQumir::TError>
MakeFilter(TOperatorPtr input, const std::string& predicate);

} // namespace NQqb
