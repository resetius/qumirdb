#include <qdb/kernel/lib.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <sstream>

namespace NQqb {
namespace NKernel {

std::expected<std::vector<NQumir::NAst::TExprPtr>, NQumir::TError>
ParseFunctionLibrary(
    const std::string& source,
    const std::unordered_set<std::string>& exclude)
{
    using namespace NQumir;
    using namespace NQumir::NAst;

    std::istringstream input(source);
    NCore::TTokenStream tokens(input);
    NCore::TParser parser;

    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    auto block = TMaybeNode<TBlockExpr>(*parsed);
    if (!block) {
        return std::unexpected(TError("kernel library: expected top-level (block ...)"));
    }

    std::vector<TExprPtr> result;
    for (auto& stmt : block.Cast()->Stmts) {
        auto fun = TMaybeNode<TFunDecl>(stmt);
        if (fun && exclude.contains(fun.Cast()->Name)) {
            continue;
        }
        result.push_back(std::move(stmt));
    }
    return result;
}

NQumir::NAst::TExprPtr MergeKernelLibrary(
    std::vector<NQumir::NAst::TExprPtr> library,
    NQumir::NAst::TExprPtr entry)
{
    using namespace NQumir;
    using namespace NQumir::NAst;

    library.push_back(std::move(entry));
    return std::make_shared<TBlockExpr>(TLocation{}, std::move(library));
}

} // namespace NKernel
} // namespace NQqb
