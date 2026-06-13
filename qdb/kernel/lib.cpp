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
    std::vector<NQumir::NAst::TExprPtr> stmts;
    if (!key.LookupTypeName.empty()) {
        stmts.push_back(std::make_shared<NQumir::NAst::TTypeDeclStmt>(
            NQumir::TLocation{}, key.LookupType));
    }
    if (!key.StoredTypeName.empty()) {
        stmts.push_back(std::make_shared<NQumir::NAst::TTypeDeclStmt>(
            NQumir::TLocation{}, key.StoredType));
    } else if (!key.IsScalar() &&
               NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(key.KeyType)) {
        stmts.push_back(std::make_shared<NQumir::NAst::TTypeDeclStmt>(
            NQumir::TLocation{}, key.KeyType));
    }

    if (key.HasDistinctLookupType()) {
        auto stringOperations = ParseFunctionLibrary(
            ReadAggregationKernel("string_ops.oz"));
        if (!stringOperations) {
            return std::unexpected(NQumir::TError(
                "string_ops.oz: " + stringOperations.error().ToString()));
        }
        stmts.insert(
            stmts.end(), stringOperations->begin(), stringOperations->end());
    }
    auto keyOperations = GenKeyOperationFunDecls(key);
    stmts.insert(stmts.end(), keyOperations.begin(), keyOperations.end());
    auto ownership = GenKeyOwnershipFunDecls(key);
    stmts.insert(stmts.end(), ownership.begin(), ownership.end());
    auto reducerDecls = GenReducerFunDecls(reducers);
    stmts.insert(stmts.end(), reducerDecls.begin(), reducerDecls.end());
    stmts.push_back(GenApplyReducersFunDecl(reducers.size()));

    auto lifecycle = ParseFunctionLibrary(
        ReadAggregationKernel("aggregation_hashtable_generic.oz"),
        {"rh_lookup_slot", "rh_insert_displace", "aht_rehash", "aht_update"});
    if (!lifecycle) {
        return std::unexpected(NQumir::TError(
            "aggregation_hashtable_generic.oz: " +
            lifecycle.error().ToString()));
    }
    stmts.insert(stmts.end(), lifecycle->begin(), lifecycle->end());

    for (const char* name : {"owned_blocks.oz", "robin_hood_dual_key.oz"}) {
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
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr columnType)
{
    auto parsed = ParseFunctionLibrary(
        ReadAggregationKernel("aggregation_finalize_states.oz"));
    if (!parsed) {
        return std::unexpected(NQumir::TError(
            "aggregation_finalize_states.oz: " + parsed.error().ToString()));
    }
    if (key.HasDistinctLookupType()) {
        auto stringOperations = ParseFunctionLibrary(
            ReadAggregationKernel("string_ops.oz"));
        if (!stringOperations) {
            return std::unexpected(NQumir::TError(
                "string_ops.oz: " + stringOperations.error().ToString()));
        }
        parsed->insert(parsed->end(),
            stringOperations->begin(), stringOperations->end());
    }
    auto entry = GenGenericAggregateFinalizeAst(
        key, std::move(hashTableType), std::move(columnType));
    auto block = NQumir::NAst::TMaybeNode<NQumir::NAst::TBlockExpr>(entry);
    if (!block || block.Cast()->Stmts.size() != 1) {
        return std::unexpected(NQumir::TError(
            "generic aggregate finalize generator returned an invalid entry block"));
    }
    if (!key.StoredTypeName.empty() ||
        (!key.IsScalar() &&
         NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(key.KeyType))) {
        parsed->insert(parsed->begin(),
            std::make_shared<NQumir::NAst::TTypeDeclStmt>(
                NQumir::TLocation{}, key.KeyType));
    }
    parsed->push_back(block.Cast()->Stmts.front());
    return std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(*parsed));
}

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateMeasureProgramAst(
    const TAggregateKeyDescriptor& key,
    NQumir::NAst::TTypePtr hashTableType)
{
    std::vector<NQumir::NAst::TExprPtr> stmts;
    if (!key.StoredTypeName.empty() ||
        (!key.IsScalar() &&
         NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(key.StoredType))) {
        stmts.push_back(std::make_shared<NQumir::NAst::TTypeDeclStmt>(
            NQumir::TLocation{}, key.StoredType));
    }
    auto entry = GenGenericAggregateMeasureAst(key, std::move(hashTableType));
    auto block = NQumir::NAst::TMaybeNode<NQumir::NAst::TBlockExpr>(entry);
    if (!block || block.Cast()->Stmts.size() != 1) {
        return std::unexpected(NQumir::TError(
            "generic aggregate measure generator returned an invalid entry block"));
    }
    stmts.push_back(block.Cast()->Stmts.front());
    return std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(stmts));
}

} // namespace NKernel
} // namespace NQqb
