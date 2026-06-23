#include <qdb/kernel/lib.h>

#include <qdb/kernel/gen.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace NQdb {
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

std::string ReadJoinKernel(const std::string& name) {
    auto path = std::filesystem::path(__FILE__).parent_path() / "join" / name;
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open join kernel: " + path.string());
    }
    std::ostringstream source;
    source << input.rdbuf();
    return source.str();
}

std::expected<std::vector<NQumir::NAst::TExprPtr>, NQumir::TError>
BuildJoinKernelLibrary() {
    std::vector<NQumir::NAst::TExprPtr> library;
    auto add = [&](const std::string& source,
        const std::unordered_set<std::string>& exclude = {})
        -> std::optional<NQumir::TError>
    {
        auto parsed = ParseFunctionLibrary(source, exclude);
        if (!parsed) {
            return parsed.error();
        }
        for (auto& fn : *parsed) {
            library.push_back(std::move(fn));
        }
        return std::nullopt;
    };

    if (auto e = add(ReadAggregationKernel("key_ops_i64.oz"))) {
        return std::unexpected(*e);
    }
    if (auto e = add(ReadAggregationKernel("robin_hood_rehash_generic.oz"))) {
        return std::unexpected(*e);
    }
    if (auto e = add(ReadAggregationKernel("aggregation_hashtable_generic.oz"),
            {"aht_update"})) {
        return std::unexpected(*e);
    }
    if (auto e = add(ReadJoinKernel("join_table.oz"))) {
        return std::unexpected(*e);
    }
    // Default always-true residual filter — must precede join_update.oz, whose
    // jt_emit_and_insert calls jt_residual_filter. CompileJoin replaces this with
    // a generated predicate when the query has a residual filter.
    if (auto e = add(ReadJoinKernel("join_residual_default.oz"))) {
        return std::unexpected(*e);
    }
    if (auto e = add(ReadJoinKernel("join_update.oz"))) {
        return std::unexpected(*e);
    }
    return library;
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
BuildFilterProgramAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType,
    std::vector<std::shared_ptr<std::string>>& literalStorage)
{
    using namespace NQumir::NAst;
    auto entry = GenFilterKernelAst(
        std::move(predicate), inputType, fieldIndices, columnType, rowSetType,
        stringViewType, literalStorage);
    auto block = TMaybeNode<TBlockExpr>(entry);
    if (!block) {
        return std::unexpected(NQumir::TError(
            "filter kernel generator returned a non-block program"));
    }

    const bool hasString = std::any_of(
        inputType.Fields.begin(), inputType.Fields.end(),
        [](const auto& field) {
            return TMaybeType<TStringType>(UnwrapNamedType(field.second));
        });
    if (!hasString) {
        return entry;
    }
    if (!stringViewType) {
        return std::unexpected(NQumir::TError(
            "filter kernel requires the StringView external type"));
    }

    return entry;
}

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateProgramAst(
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
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
    } else if (NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(key.KeyType)) {
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
    auto reducerDecls = GenReducerFunDecls(layout);
    stmts.insert(stmts.end(), reducerDecls.begin(), reducerDecls.end());
    stmts.push_back(GenApplyReducersFunDecl(layout));

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
        inputType, key, layout, std::move(columnType),
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
    const TAggReducerLayout& layout,
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
        key, layout, std::move(hashTableType), std::move(columnType));
    auto block = NQumir::NAst::TMaybeNode<NQumir::NAst::TBlockExpr>(entry);
    if (!block || block.Cast()->Stmts.size() != 1) {
        return std::unexpected(NQumir::TError(
            "generic aggregate finalize generator returned an invalid entry block"));
    }
    if (!key.StoredTypeName.empty() ||
        NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(key.KeyType)) {
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
        NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(key.StoredType)) {
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
} // namespace NQdb
