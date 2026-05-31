#include <qdb/ops/filter.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>

#include <sstream>

namespace NQqb {

TFilterOperator::TFilterOperator(TOperatorPtr input, NQumir::NAst::TExprPtr predicate)
    : Input_(std::move(input))
    , Predicate_(std::move(predicate))
{
    Type = Input_->Type;
}

const std::string TFilterOperator::ToString() const {
    using namespace NQumir::NAst::NCore;
    return "(rel filter " + Input_->ToString() + " " + PrintAst(Predicate_) + ")";
}

std::expected<TOperatorPtr, NQumir::TError>
MakeFilter(TOperatorPtr input, const std::string& predicate) {
    std::istringstream ss(predicate);
    NQumir::NAst::NCore::TTokenStream tokens(ss);
    NQumir::NAst::NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return std::make_shared<TFilterOperator>(std::move(input), std::move(*parsed));
}

} // namespace NQqb
