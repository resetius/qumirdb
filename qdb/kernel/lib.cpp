#include <qdb/kernel/lib.h>

#include <qdb/kernel/gen.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace NQqb {
namespace NKernel {

std::string ReadAggregationKernel(const std::string& name) {
    auto path = std::filesystem::path(__FILE__).parent_path() / "aggregation" / name;
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open aggregation kernel: " + path.string());
    }
    std::ostringstream source;
    source << input.rdbuf();
    return source.str();
}

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

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateProgramAst(
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const std::optional<std::string>& argField,
    const std::vector<std::string>& reducers,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType)
{
    std::vector<NQumir::NAst::TExprPtr> stmts = GenKeyOperationFunDecls(key);
    auto reducerDecls = GenReducerFunDecls(reducers);
    stmts.insert(stmts.end(), reducerDecls.begin(), reducerDecls.end());
    stmts.push_back(GenApplyReducersFunDecl(reducers.size()));

    for (const char* name : {
             "robin_hood_rehash_generic.oz",
             "aggregation_hashtable_generic.oz",
         }) {
        auto parsed = ParseFunctionLibrary(ReadAggregationKernel(name));
        if (!parsed) {
            return std::unexpected(NQumir::TError(
                std::string(name) + ": " + parsed.error().ToString()));
        }
        stmts.insert(stmts.end(), parsed->begin(), parsed->end());
    }

    auto entry = GenGenericAggregateDispatchAst(
        inputType, key, argField, reducers.size(), std::move(columnType),
        std::move(rowSetType), std::move(hashTableType));
    auto block = NQumir::NAst::TMaybeNode<NQumir::NAst::TBlockExpr>(entry);
    if (!block || block.Cast()->Stmts.size() != 1) {
        return std::unexpected(NQumir::TError(
            "generic aggregate dispatch generator returned an invalid entry block"));
    }
    stmts.push_back(block.Cast()->Stmts.front());
    return std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(stmts));
}

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateFinalizeProgramAst(
    const TAggregateKeyDescriptor& key,
    NQumir::NAst::TTypePtr hashTableType)
{
    auto parsed = ParseFunctionLibrary(
        ReadAggregationKernel("aggregation_finalize_generic.oz"));
    if (!parsed) {
        return std::unexpected(NQumir::TError(
            "aggregation_finalize_generic.oz: " + parsed.error().ToString()));
    }
    auto entry = GenGenericAggregateFinalizeAst(key, std::move(hashTableType));
    auto block = NQumir::NAst::TMaybeNode<NQumir::NAst::TBlockExpr>(entry);
    if (!block || block.Cast()->Stmts.size() != 1) {
        return std::unexpected(NQumir::TError(
            "generic aggregate finalize generator returned an invalid entry block"));
    }
    parsed->push_back(block.Cast()->Stmts.front());
    return std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(*parsed));
}

} // namespace NKernel
} // namespace NQqb
