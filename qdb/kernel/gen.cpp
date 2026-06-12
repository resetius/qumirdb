#include <qdb/kernel/gen.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/operator.h>
#include <qumir/location.h>

#include <algorithm>
#include <stdexcept>

namespace NQqb {
namespace NKernel {

const std::unordered_set<std::string> kCountOzFixedFuncs = {
    "agg_count_step", "agg_sum_i64_step", "agg_min_i64_step", "agg_max_i64_step",
    "count_init", "count_rehash", "count_update", "count_destroy",
    "aggregate_batch", "aggregation_count"};

std::vector<NQumir::NAst::TExprPtr> GenKeyOperationFunDecls(
    const TAggregateKeyDescriptor& key)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto integer = TMaybeType<TIntegerType>(UnwrapNamedType(key.KeyType));
    if (!key.IsScalar() || !integer || integer.Cast()->Kind != TIntegerType::I64) {
        throw std::invalid_argument(
            "GenKeyOperationFunDecls: only scalar i64 keys are implemented");
    }

    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto boolType = std::make_shared<TBoolType>();
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value, const TTypePtr& type) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = type;
        return result;
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    auto assignHash = [&](TExprPtr value) -> TExprPtr {
        return std::make_shared<TAssignExpr>(loc, "h", std::move(value));
    };

    std::vector<TExprPtr> hashBody;
    hashBody.push_back(std::make_shared<TVarStmt>(loc, "h", u64Type));
    hashBody.push_back(assignHash(std::make_shared<TCastExpr>(loc, ident("key"), u64Type)));
    hashBody.push_back(assignHash(binary("xor", ident("h"),
        binary(">>", ident("h"), number(12, u64Type)))));
    hashBody.push_back(assignHash(binary("xor", ident("h"),
        binary("<<", ident("h"), number(25, u64Type)))));
    hashBody.push_back(assignHash(binary("xor", ident("h"),
        binary(">>", ident("h"), number(27, u64Type)))));
    hashBody.push_back(assignHash(binary("*", ident("h"),
        number(2685821657736338717LL, u64Type))));
    hashBody.push_back(std::make_shared<TReturnExpr>(loc,
        std::make_shared<TCastExpr>(loc, ident("h"), i64Type)));

    std::vector<TParam> hashParams = {
        std::make_shared<TVarStmt>(loc, "key", key.KeyType),
    };
    auto hash = std::make_shared<TFunDecl>(loc, "rh_hash", std::move(hashParams),
        std::make_shared<TBlockExpr>(loc, std::move(hashBody)), i64Type);
    hash->Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{key.KeyType}, i64Type);

    std::vector<TParam> equalParams = {
        std::make_shared<TVarStmt>(loc, "left", key.KeyType),
        std::make_shared<TVarStmt>(loc, "right", key.KeyType),
    };
    std::vector<TExprPtr> equalBody = {
        std::make_shared<TReturnExpr>(loc,
            binary("==", ident("left"), ident("right"))),
    };
    auto equal = std::make_shared<TFunDecl>(loc, "rh_key_equal", std::move(equalParams),
        std::make_shared<TBlockExpr>(loc, std::move(equalBody)), boolType);
    equal->Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{key.KeyType, key.KeyType}, boolType);

    return {std::move(hash), std::move(equal)};
}

void SubstFieldsInPlace(
    NQumir::NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& fieldNames,
    const NQumir::NAst::TExprPtr& indexIdent)
{
    if (!expr) {
        return;
    }
    if (auto node = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(expr)) {
        if (fieldNames.count(node.Cast()->Name)) {
            expr = std::make_shared<NQumir::NAst::TIndexExpr>(
                expr->Location, expr, indexIdent);
            return;
        }
        return;
    }
    for (auto* child : expr->MutableChildren()) {
        SubstFieldsInPlace(*child, fieldNames, indexIdent);
    }
}

// Kernel takes (ref TRowSet) directly. Column data pointers are extracted via
// TRowSet.Columns[colIdx].Data with a two-step cast: <ptr i8> -> i64 -> <ptr T>.
NQumir::NAst::TExprPtr GenFilterKernelAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    std::unordered_set<std::string> fieldNames;
    for (const auto& [name, _] : inputType.Fields) {
        fieldNames.insert(name);
    }

    auto identI = std::make_shared<TIdentExpr>(loc, "i");
    SubstFieldsInPlace(predicate, fieldNames, identI);

    // Single param: (var rowSet <ref TRowSet>) — raw struct type, no TNamedType wrapper
    auto rowSetRefType = std::make_shared<TReferenceType>(rowSetType);
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "rowSet", rowSetRefType),
    };

    std::vector<TExprPtr> bodyStmts;
    auto identRowSet = std::make_shared<TIdentExpr>(loc, "rowSet");

    auto fieldOf = [&](const std::string& name) {
        return std::make_shared<TFieldAccessExpr>(loc, identRowSet, name);
    };

    // Extract n, selection, cols from rowSet
    bodyStmts.push_back(std::make_shared<TVarStmt>(loc, "n", std::make_shared<TIntegerType>()));
    bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, "n", fieldOf("RowCount")));

    auto ptrU8Type = std::make_shared<TPointerType>(
        std::make_shared<TIntegerType>(TIntegerType::U8));
    bodyStmts.push_back(std::make_shared<TVarStmt>(loc, "selection", ptrU8Type));
    bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, "selection", fieldOf("Selection")));

    // Use the exact Columns field type from rowSetType to avoid type mismatches.
    auto ptrColumnType = [&]() -> TTypePtr {
        auto* rs = static_cast<TStructType*>(rowSetType.get());
        for (const auto& [name, type] : rs->Fields) {
            if (name == "Columns") {
                return type;
            }
        }
        return std::make_shared<TPointerType>(columnType);
    }();
    bodyStmts.push_back(std::make_shared<TVarStmt>(loc, "cols", ptrColumnType));
    bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, "cols", fieldOf("Columns")));

    // For each referenced field: cols[colIdx].Data -> i64 -> <ptr T>
    auto i64Type = std::make_shared<TIntegerType>();
    for (const auto& [name, type] : inputType.Fields) {
        int32_t idx = fieldIndices.at(name);
        auto ptrFieldType = std::make_shared<TPointerType>(type);
        auto colElem = std::make_shared<TIndexExpr>(loc,
            std::make_shared<TIdentExpr>(loc, "cols"),
            std::make_shared<TNumberExpr>(loc, int64_t(idx)));
        auto rawData = std::make_shared<TFieldAccessExpr>(loc, colElem, "Data");
        auto asInt = std::make_shared<TCastExpr>(loc, rawData, i64Type);
        auto asTypedPtr = std::make_shared<TCastExpr>(loc, asInt, ptrFieldType);

        bodyStmts.push_back(std::make_shared<TVarStmt>(loc, name, ptrFieldType));
        bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, name, asTypedPtr));
    }

    // Loop
    auto varI = std::make_shared<TVarStmt>(loc, "i", std::make_shared<TIntegerType>());
    auto initI = std::make_shared<TAssignExpr>(loc, "i",
        std::make_shared<TNumberExpr>(loc, int64_t(0)));
    auto cond = std::make_shared<TBinaryExpr>(loc, TOperator("<"),
        std::make_shared<TIdentExpr>(loc, "i"),
        std::make_shared<TIdentExpr>(loc, "n"));
    auto castedPred = std::make_shared<TCastExpr>(loc, predicate,
        std::make_shared<TIntegerType>(TIntegerType::U8));
    auto writeSel = std::make_shared<TArrayAssignExpr>(loc, "selection",
        std::vector<TExprPtr>{std::make_shared<TIdentExpr>(loc, "i")},
        castedPred);
    auto incrI = std::make_shared<TAssignExpr>(loc, "i",
        std::make_shared<TBinaryExpr>(loc, TOperator("+"),
            std::make_shared<TIdentExpr>(loc, "i"),
            std::make_shared<TNumberExpr>(loc, int64_t(1))));

    bodyStmts.push_back(varI);
    bodyStmts.push_back(initI);
    bodyStmts.push_back(std::make_shared<TWhileStmtExpr>(loc, cond,
        std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{writeSel, incrI})));

    auto funBody = std::make_shared<TBlockExpr>(loc, std::move(bodyStmts));
    auto funDecl = std::make_shared<TFunDecl>(loc, "<kernel>",
        std::move(params), funBody, std::make_shared<TVoidType>());

    return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{funDecl});
}

NQumir::NAst::TExprPtr GenAggregateKernelAst(
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    const std::string& keyField,
    const std::optional<std::string>& argField,
    size_t numAggs,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);

    auto ident = [&](const std::string& name) {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto numberOf = [&](int64_t value, TTypePtr type) -> TExprPtr {
        auto expr = std::make_shared<TNumberExpr>(loc, value);
        expr->Type = std::move(type);
        return expr;
    };
    auto numI64 = [&](int64_t value) { return numberOf(value, i64Type); };
    auto varDecl = [&](const std::string& name, TTypePtr type) {
        return std::make_shared<TVarStmt>(loc, name, std::move(type));
    };
    auto assign = [&](const std::string& name, TExprPtr value) -> TExprPtr {
        return std::make_shared<TAssignExpr>(loc, name, std::move(value));
    };
    auto binary = [&](const char* op, TExprPtr l, TExprPtr r) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(loc, TOperator(op), std::move(l), std::move(r));
    };
    auto call = [&](const std::string& name, std::vector<TExprPtr> args) -> TExprPtr {
        return std::make_shared<TCallExpr>(loc, ident(name), std::move(args));
    };
    auto castTo = [&](TExprPtr expr, TTypePtr type) -> TExprPtr {
        return std::make_shared<TCastExpr>(loc, std::move(expr), std::move(type));
    };
    auto block = [&](std::vector<TExprPtr> stmts) {
        return std::make_shared<TBlockExpr>(loc, std::move(stmts));
    };
    auto fieldOf = [&](const std::string& object, const std::string& field) -> TExprPtr {
        return std::make_shared<TFieldAccessExpr>(loc, ident(object), field);
    };

    // ht/batch must be <ref HashTable>/<ref TRowSet> as seen by count.oz's
    // text-parsed functions, i.e. TReferenceType(TNamedType(name, type)) —
    // matching how the resolver turns a parsed "<ref HashTable>" annotation
    // into TReferenceType(TNamedType("HashTable", UnderlyingType=hashTableType)).
    // A raw TReferenceType(hashTableType) has TypeName()=="Struct" and fails
    // EqualTypes against the "Named" type expected by count_init/count_update.
    auto hashTableRefType = std::make_shared<TReferenceType>(
        std::make_shared<TNamedType>("HashTable", hashTableType));
    auto rowSetRefType = std::make_shared<TReferenceType>(
        std::make_shared<TNamedType>("TRowSet", rowSetType));
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "ht", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "arg", i64Type),
        std::make_shared<TVarStmt>(loc, "op", i64Type),
    };

    // ---- op == 1: update batch ----
    std::vector<TExprPtr> updateStmts;

    updateStmts.push_back(varDecl("n", i64Type));
    updateStmts.push_back(assign("n", fieldOf("batch", "RowCount")));

    updateStmts.push_back(varDecl("selection", ptrU8Type));
    updateStmts.push_back(assign("selection", fieldOf("batch", "Selection")));

    // Use the exact Columns field type from rowSetType to avoid type mismatches.
    auto ptrColumnType = [&]() -> TTypePtr {
        auto* rs = static_cast<TStructType*>(rowSetType.get());
        for (const auto& [name, type] : rs->Fields) {
            if (name == "Columns") {
                return type;
            }
        }
        return std::make_shared<TPointerType>(columnType);
    }();
    updateStmts.push_back(varDecl("cols", ptrColumnType));
    updateStmts.push_back(assign("cols", fieldOf("batch", "Columns")));

    // cols[idx].Data -> i64 -> <ptr i64> (Stage 1: i64 keys/args only).
    auto colDataAsPtrI64 = [&](int32_t idx) -> TExprPtr {
        auto colElem = std::make_shared<TIndexExpr>(loc, ident("cols"), numI64(idx));
        auto rawData = std::make_shared<TFieldAccessExpr>(loc, colElem, "Data");
        return castTo(castTo(rawData, i64Type), ptrI64Type);
    };

    updateStmts.push_back(varDecl("keys", ptrI64Type));
    updateStmts.push_back(assign("keys", colDataAsPtrI64(fieldIndices.at(keyField))));

    if (argField) {
        updateStmts.push_back(varDecl("values", ptrI64Type));
        updateStmts.push_back(assign("values", colDataAsPtrI64(fieldIndices.at(*argField))));
    }

    // selection_is_null = (cast selection i64) == 0
    updateStmts.push_back(varDecl("selection_is_null", boolType));
    updateStmts.push_back(assign("selection_is_null",
        binary("==", castTo(ident("selection"), i64Type), numI64(0))));

    updateStmts.push_back(varDecl("dense_slot", i64Type));
    updateStmts.push_back(varDecl("value", i64Type));
    updateStmts.push_back(varDecl("i", i64Type));
    updateStmts.push_back(assign("i", numI64(0)));

    // shouldProcess = selection_is_null || selection[i] != 0
    auto shouldProcess = binary("||",
        ident("selection_is_null"),
        binary("!=",
            std::make_shared<TIndexExpr>(loc, ident("selection"), ident("i")),
            numberOf(0, u8Type)));

    TExprPtr valueExpr = argField
        ? std::make_shared<TIndexExpr>(loc, ident("values"), ident("i"))
        : numI64(0);

    auto processRow = block({
        assign("value", valueExpr),
        assign("dense_slot", call("agg_update", {
            ident("ht"),
            std::make_shared<TIndexExpr>(loc, ident("keys"), ident("i")),
            ident("value"),
        })),
    });

    auto loopBody = block({
        std::make_shared<TIfExpr>(loc, shouldProcess, processRow, nullptr),
        assign("i", binary("+", ident("i"), numI64(1))),
    });
    updateStmts.push_back(std::make_shared<TWhileStmtExpr>(loc,
        binary("<", ident("i"), ident("n")), loopBody));

    updateStmts.push_back(numI64(0));
    auto updateBranch = block(std::move(updateStmts));

    // ---- op == 0: init ----
    auto initBranch = castTo(call("agg_init", {
        ident("ht"), ident("arg"), numI64(static_cast<int64_t>(numAggs)),
    }), i64Type);

    // ---- otherwise: destroy ----
    auto destroyBranch = block({
        call("agg_destroy", {ident("ht")}),
        numI64(1),
    });

    auto dispatch = std::make_shared<TIfExpr>(loc,
        binary("==", ident("op"), numI64(0)),
        initBranch,
        std::make_shared<TIfExpr>(loc,
            binary("==", ident("op"), numI64(1)),
            updateBranch,
            destroyBranch));

    auto funBody = block({ std::make_shared<TReturnExpr>(loc, dispatch) });
    auto funDecl = std::make_shared<TFunDecl>(loc, "agg_dispatch",
        std::move(params), funBody, i64Type);

    return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{funDecl});
}

NQumir::NAst::TExprPtr GenGenericAggregateDispatchAst(
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const std::optional<std::string>& argField,
    size_t numAggs,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    if (!key.IsScalar()) {
        throw std::invalid_argument(
            "GenGenericAggregateDispatchAst: composite keys are not implemented yet");
    }

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrKeyType = std::make_shared<TPointerType>(key.KeyType);
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);

    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value, TTypePtr type) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = std::move(type);
        return result;
    };
    auto numI64 = [&](int64_t value) { return number(value, i64Type); };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    auto call = [&](const std::string& name, std::vector<TExprPtr> args) -> TExprPtr {
        return std::make_shared<TCallExpr>(loc, ident(name), std::move(args));
    };
    auto cast = [&](TExprPtr expr, TTypePtr type) -> TExprPtr {
        return std::make_shared<TCastExpr>(loc, std::move(expr), std::move(type));
    };
    auto assign = [&](const std::string& name, TExprPtr value) -> TExprPtr {
        return std::make_shared<TAssignExpr>(loc, name, std::move(value));
    };
    auto var = [&](const std::string& name, TTypePtr type) -> TExprPtr {
        return std::make_shared<TVarStmt>(loc, name, std::move(type));
    };
    auto block = [&](std::vector<TExprPtr> stmts) -> TExprPtr {
        return std::make_shared<TBlockExpr>(loc, std::move(stmts));
    };
    auto field = [&](const std::string& object, const std::string& name) -> TExprPtr {
        return std::make_shared<TFieldAccessExpr>(loc, ident(object), name);
    };

    auto hashTableRefType = std::make_shared<TReferenceType>(
        std::make_shared<TNamedType>("HashTable", hashTableType));
    auto rowSetRefType = std::make_shared<TReferenceType>(
        std::make_shared<TNamedType>("TRowSet", rowSetType));
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "ht", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "arg", i64Type),
        std::make_shared<TVarStmt>(loc, "op", i64Type),
    };

    auto ptrColumnType = [&]() -> TTypePtr {
        auto* rowSet = static_cast<TStructType*>(rowSetType.get());
        for (const auto& [name, type] : rowSet->Fields) {
            if (name == "Columns") {
                return type;
            }
        }
        return std::make_shared<TPointerType>(columnType);
    }();
    auto columnData = [&](int32_t index, TTypePtr pointerType) -> TExprPtr {
        auto column = std::make_shared<TIndexExpr>(
            loc, ident("cols"), numI64(index));
        auto data = std::make_shared<TFieldAccessExpr>(loc, column, "Data");
        return cast(cast(data, i64Type), std::move(pointerType));
    };

    std::vector<TExprPtr> update;
    update.push_back(var("n", i64Type));
    update.push_back(assign("n", field("batch", "RowCount")));
    update.push_back(var("selection", ptrU8Type));
    update.push_back(assign("selection", field("batch", "Selection")));
    update.push_back(var("cols", ptrColumnType));
    update.push_back(assign("cols", field("batch", "Columns")));
    update.push_back(var("keys", ptrKeyType));
    update.push_back(assign("keys", columnData(key.Fields.front().ColumnIndex, ptrKeyType)));
    if (argField) {
        auto arg = std::find_if(inputType.Fields.begin(), inputType.Fields.end(),
            [&](const auto& item) { return item.first == *argField; });
        if (arg == inputType.Fields.end()) {
            throw std::invalid_argument(
                "GenGenericAggregateDispatchAst: unknown argument column '" + *argField + "'");
        }
        auto argType = TMaybeType<TIntegerType>(UnwrapNamedType(arg->second));
        if (!argType || argType.Cast()->Kind != TIntegerType::I64) {
            throw std::invalid_argument(
                "GenGenericAggregateDispatchAst: aggregate argument must be i64");
        }
        const auto index = static_cast<int32_t>(std::distance(inputType.Fields.begin(), arg));
        update.push_back(var("values", ptrI64Type));
        update.push_back(assign("values", columnData(index, ptrI64Type)));
    }
    update.push_back(var("selection_is_null", boolType));
    update.push_back(assign("selection_is_null",
        binary("==", cast(ident("selection"), i64Type), numI64(0))));
    update.push_back(var("dense_slot", i64Type));
    update.push_back(var("i", i64Type));
    update.push_back(assign("i", numI64(0)));

    auto selected = binary("||", ident("selection_is_null"),
        binary("!=", std::make_shared<TIndexExpr>(loc, ident("selection"), ident("i")),
            number(0, u8Type)));
    TExprPtr value = argField
        ? std::make_shared<TIndexExpr>(loc, ident("values"), ident("i"))
        : numI64(0);
    auto updateCall = call("aht_update", {
        ident("ht"),
        std::make_shared<TIndexExpr>(loc, ident("keys"), ident("i")),
        std::move(value),
    });
    auto process = block({assign("dense_slot", std::move(updateCall))});
    auto loop = block({
        std::make_shared<TIfExpr>(loc, std::move(selected), std::move(process), nullptr),
        assign("i", binary("+", ident("i"), numI64(1))),
    });
    update.push_back(std::make_shared<TWhileStmtExpr>(
        loc, binary("<", ident("i"), ident("n")), std::move(loop)));
    update.push_back(numI64(0));

    auto init = cast(call("aht_init", {
        ident("ht"), ident("arg"), numI64(static_cast<int64_t>(numAggs)),
        numI64(static_cast<int64_t>(key.Size)),
    }), i64Type);
    auto destroy = block({call("aht_destroy", {ident("ht")}), numI64(1)});
    auto dispatch = std::make_shared<TIfExpr>(loc,
        binary("==", ident("op"), numI64(0)), std::move(init),
        std::make_shared<TIfExpr>(loc,
            binary("==", ident("op"), numI64(1)), block(std::move(update)),
            std::move(destroy)));
    auto body = std::make_shared<TBlockExpr>(loc,
        std::vector<TExprPtr>{std::make_shared<TReturnExpr>(loc, dispatch)});
    auto function = std::make_shared<TFunDecl>(
        loc, "agg_dispatch", std::move(params), std::move(body), i64Type);
    return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{function});
}

std::vector<NQumir::NAst::TExprPtr> GenReducerFunDecls(
    const std::vector<std::string>& funcs)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto boolType = std::make_shared<TBoolType>();

    auto ident = [&](const std::string& name) {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto numI64 = [&](int64_t value) -> TExprPtr {
        auto expr = std::make_shared<TNumberExpr>(loc, value);
        expr->Type = i64Type;
        return expr;
    };
    auto binary = [&](const char* op, TExprPtr l, TExprPtr r) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(loc, TOperator(op), std::move(l), std::move(r));
    };

    std::vector<TExprPtr> result;
    result.reserve(funcs.size());
    for (size_t i = 0; i < funcs.size(); ++i) {
        std::vector<TParam> params = {
            std::make_shared<TVarStmt>(loc, "prev", i64Type),
            std::make_shared<TVarStmt>(loc, "value", i64Type),
            std::make_shared<TVarStmt>(loc, "is_new", boolType),
        };

        TExprPtr resultExpr;
        const std::string& func = funcs[i];
        if (func == "count") {
            resultExpr = binary("+", ident("prev"), numI64(1));
        } else if (func == "sum") {
            resultExpr = binary("+", ident("prev"), ident("value"));
        } else if (func == "min") {
            resultExpr = std::make_shared<TIfExpr>(loc, ident("is_new"), ident("value"),
                std::make_shared<TIfExpr>(loc, binary("<", ident("value"), ident("prev")),
                    ident("value"), ident("prev")));
        } else if (func == "max") {
            resultExpr = std::make_shared<TIfExpr>(loc, ident("is_new"), ident("value"),
                std::make_shared<TIfExpr>(loc, binary(">", ident("value"), ident("prev")),
                    ident("value"), ident("prev")));
        } else {
            throw std::invalid_argument("GenReducerFunDecls: unknown aggregate function: " + func);
        }

        auto body = std::make_shared<TBlockExpr>(loc,
            std::vector<TExprPtr>{ std::make_shared<TReturnExpr>(loc, std::move(resultExpr)) });
        result.push_back(std::make_shared<TFunDecl>(loc, "reduce_" + std::to_string(i),
            std::move(params), std::move(body), i64Type));
    }
    return result;
}

NQumir::NAst::TExprPtr GenApplyReducersFunDecl(size_t numReducers)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto boolType = std::make_shared<TBoolType>();
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);

    auto ident = [&](const std::string& name) {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto numI64 = [&](int64_t value) -> TExprPtr {
        auto expr = std::make_shared<TNumberExpr>(loc, value);
        expr->Type = i64Type;
        return expr;
    };

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "agg_buffers", ptrPtrI64Type),
        std::make_shared<TVarStmt>(loc, "dense_slot", i64Type),
        std::make_shared<TVarStmt>(loc, "value", i64Type),
        std::make_shared<TVarStmt>(loc, "is_new", boolType),
    };

    std::vector<TExprPtr> stmts;
    for (size_t i = 0; i < numReducers; ++i) {
        std::string bufName = "buf_" + std::to_string(i);
        stmts.push_back(std::make_shared<TVarStmt>(loc, bufName, ptrI64Type));
        stmts.push_back(std::make_shared<TAssignExpr>(loc, bufName,
            std::make_shared<TIndexExpr>(loc, ident("agg_buffers"), numI64(static_cast<int64_t>(i)))));

        auto prev = std::make_shared<TIndexExpr>(loc, ident(bufName), ident("dense_slot"));
        auto call = std::make_shared<TCallExpr>(loc, ident("reduce_" + std::to_string(i)),
            std::vector<TExprPtr>{prev, ident("value"), ident("is_new")});
        stmts.push_back(std::make_shared<TArrayAssignExpr>(loc, bufName,
            std::vector<TExprPtr>{ident("dense_slot")}, call));
    }

    auto body = std::make_shared<TBlockExpr>(loc, std::move(stmts));
    return std::make_shared<TFunDecl>(loc, "agg_apply_reducers",
        std::move(params), std::move(body), std::make_shared<TVoidType>());
}

} // namespace NKernel
} // namespace NQqb
