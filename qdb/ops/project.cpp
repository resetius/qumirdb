#include <qdb/ops/project.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <sstream>

namespace NQqb {

TProjectOperator::TProjectOperator(TOperatorPtr input, std::vector<TProjectionSpec> projections)
    : Input_(std::move(input))
    , Projections_(std::move(projections))
{
    auto* inputType = static_cast<NQumir::NAst::TStructType*>(Input_->Type.get());
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> outFields;
    if (inputType) {
        for (const auto& proj : Projections_) {
            NQumir::NAst::TTypePtr fieldType;
            // For a simple identity projection (ident expr), resolve type from input.
            if (auto ident = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(proj.Expression)) {
                for (const auto& [name, type] : inputType->Fields) {
                    if (name == ident.Cast()->Name) {
                        fieldType = type;
                        break;
                    }
                }
            }
            outFields.emplace_back(proj.Name, fieldType);
        }
    }
    Type = std::make_shared<NQumir::NAst::TStructType>(std::move(outFields));
}

const std::string TProjectOperator::ToString() const {
    std::string s = "(rel project " + Input_->ToString();
    for (const auto& p : Projections_) {
        s += " (" + p.Name + " " + NQumir::NAst::NCore::PrintAst(p.Expression) + ")";
    }
    return s + ")";
}

std::expected<TOperatorPtr, NQumir::TError>
MakeProject(TOperatorPtr input, std::vector<std::pair<std::string, std::string>> projections) {
    std::vector<TProjectionSpec> specs;
    specs.reserve(projections.size());
    for (auto& [name, exprStr] : projections) {
        std::istringstream ss(exprStr);
        NQumir::NAst::NCore::TTokenStream tokens(ss);
        NQumir::NAst::NCore::TParser parser;
        auto parsed = parser.Parse(tokens);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        specs.push_back({std::move(name), std::move(*parsed)});
    }
    return std::make_shared<TProjectOperator>(std::move(input), std::move(specs));
}

} // namespace NQqb
