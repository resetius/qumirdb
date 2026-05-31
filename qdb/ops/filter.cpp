#include <qdb/ops/filter.h>
#include <qdb/ops/parse_kernel.h>

#include <qumir/parser/ast.h>

namespace NQqb {

TFilterOperator::TFilterOperator(TOperatorPtr input, std::string predicate, NQumir::NAst::TExprPtr kernel)
    : Input_(std::move(input))
    , Predicate_(std::move(predicate))
    , Kernel_(std::move(kernel))
{
    Type = Input_->Type; // filter doesn't change schema
}

const std::string TFilterOperator::ToString() const {
    auto* fun = static_cast<NQumir::NAst::TFunDecl*>(Kernel_.get());
    std::string bodyStr = fun && fun->Body ? fun->Body->ToString() : "?";
    return "(rel filter " + Input_->ToString() + " " + bodyStr + ")";
}

std::expected<TOperatorPtr, NQumir::TError>
MakeFilter(TOperatorPtr input, const std::string& predicate) {
    using namespace NQumir::NAst;

    auto* structType = static_cast<TStructType*>(input->Type.get());
    if (!structType) {
        return std::unexpected(NQumir::TError("filter input must have TStructType"));
    }

    auto kernel = NInternal::ParseAndAnnotate(*structType, predicate);
    if (!kernel) {
        return std::unexpected(kernel.error());
    }

    return std::make_shared<TFilterOperator>(std::move(input), predicate, std::move(*kernel));
}

} // namespace NQqb
