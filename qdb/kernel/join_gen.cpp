#include <qdb/kernel/join_gen.h>

#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/column_value.h>
#include <qdb/kernel/gen.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>

namespace NQdb::NKernel {

using namespace NQumir::NAst;

std::vector<NQumir::NAst::TExprPtr> GenJoinKeyTypeDecls(const TJoinKeyDescriptor& key) {
    NQumir::TLocation loc{};
    std::vector<NQumir::NAst::TExprPtr> stmts;
    if (!key.LookupTypeName.empty()) {
        stmts.push_back(std::make_shared<TTypeDeclStmt>(loc, key.LookupType));
    }
    if (!key.StoredTypeName.empty()) {
        stmts.push_back(std::make_shared<TTypeDeclStmt>(loc, key.StoredType));
    } else if (TMaybeType<TNamedType>(key.KeyType)) {
        stmts.push_back(std::make_shared<TTypeDeclStmt>(loc, key.KeyType));
    }
    return stmts;
}

std::vector<NQumir::NAst::TExprPtr> GenJoinKeyOpsFunDecls(const TJoinKeyDescriptor& key) {
    // GenKeyOperationFunDecls only reads the type fields, so a thin shim suffices.
    TAggregateKeyDescriptor shim;
    shim.TypeName = key.TypeName;
    shim.LookupTypeName = key.LookupTypeName;
    shim.StoredTypeName = key.StoredTypeName;
    shim.KeyType = key.KeyType;
    shim.LookupType = key.LookupType;
    shim.StoredType = key.StoredType;
    shim.Size = key.Size;
    shim.Alignment = key.Alignment;
    return GenKeyOperationFunDecls(shim);
}

NQumir::NAst::TExprPtr GenJoinInsertKeyOnlyAst(
    const TJoinKeyDescriptor& key,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType)
{
    NQumir::TLocation loc{};

    if (key.HasDistinctLookupType()) {
        throw NQumir::TError(
            "GenJoinInsertKeyOnlyAst: string keys not supported yet");
    }

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);

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
    auto pairBufferRefType = std::make_shared<TReferenceType>(
        std::make_shared<TNamedType>("PairBuffer", pairBufferType));

    // Same ABI as GenJoinProcessAst: (own, opp, batch, batch_idx, pairs) -> bool
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "own", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "opp", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "batch_idx", i64Type),
        std::make_shared<TVarStmt>(loc, "pairs", pairBufferRefType),
    };

    auto ptrColumnType = [&]() -> TTypePtr {
        auto* rowSet = static_cast<TStructType*>(rowSetType.get());
        for (const auto& [name, type] : rowSet->Fields) {
            if (name == "Columns") return type;
        }
        return std::make_shared<TPointerType>(columnType);
    }();
    auto columnValueType = [&]() -> TTypePtr {
        if (auto pointer = TMaybeType<TPointerType>(ptrColumnType)) {
            return pointer.Cast()->PointeeType;
        }
        return columnType;
    }();
    auto columnAt = [&](int32_t index) -> TExprPtr {
        return std::make_shared<TIndexExpr>(loc, ident("cols"), numI64(index));
    };

    std::vector<TExprPtr> body;
    body.push_back(var("n", i64Type));
    body.push_back(assign("n", field("batch", "RowCount")));
    body.push_back(var("selection", ptrU8Type));
    body.push_back(assign("selection", field("batch", "Selection")));
    body.push_back(var("cols", ptrColumnType));
    body.push_back(assign("cols", field("batch", "Columns")));
    for (size_t fi = 0; fi < key.Fields.size(); ++fi) {
        const int32_t colIdx = key.Fields[fi].RightColumnIndex; // right side
        const std::string name = "key_column_" + std::to_string(fi);
        body.push_back(var(name, columnValueType));
        body.push_back(assign(name, columnAt(colIdx)));
    }
    body.push_back(var("selection_is_null", boolType));
    body.push_back(assign("selection_is_null",
        binary("==", cast(ident("selection"), i64Type), numI64(0))));
    body.push_back(var("i", i64Type));
    body.push_back(assign("i", numI64(0)));

    std::vector<TColumnValueAst> keyFields;
    keyFields.reserve(key.Fields.size());
    for (size_t fi = 0; fi < key.Fields.size(); ++fi) {
        auto logicalType = key.Fields[fi].IsNullable
            ? std::make_shared<TNullable>(key.Fields[fi].Type)
            : key.Fields[fi].Type;
        keyFields.push_back(BuildColumnValueAst(
            "key_column_" + std::to_string(fi), "i",
            "key_value_" + std::to_string(fi),
            std::move(logicalType), nullptr));
    }

    auto namedKey = TMaybeType<TNamedType>(key.KeyType);
    auto keyStruct = namedKey
        ? TMaybeType<TStructType>(namedKey.Cast()->UnderlyingType)
        : TMaybeType<TStructType>(key.KeyType);
    if (!keyStruct) {
        throw NQumir::TError("GenJoinInsertKeyOnlyAst: key must be a struct");
    }
    std::vector<TExprPtr> structFields;
    structFields.reserve(keyStruct.Cast()->Fields.size());
    for (const auto& [fieldName, fieldType] : keyStruct.Cast()->Fields) {
        if (fieldName.starts_with("__qdb_padding_")) {
            structFields.push_back(number(0, fieldType));
            continue;
        }
        const bool validity = fieldName.starts_with("valid_");
        const std::string_view prefix = validity ? "valid_" : "key_";
        const size_t fieldIndex = std::stoull(fieldName.substr(prefix.size()));
        structFields.push_back(validity
            ? keyFields[fieldIndex].IsValid
            : keyFields[fieldIndex].Value);
    }
    TExprPtr keyValue = std::make_shared<TStructConstructExpr>(
        loc, key.KeyType, std::move(structFields));

    auto insertCall = call("jt_insert_slot_only", {ident("own"), std::move(keyValue)});

    std::vector<TExprPtr> process;
    for (auto& kf : keyFields) {
        process.insert(process.end(),
            std::make_move_iterator(kf.Setup.begin()),
            std::make_move_iterator(kf.Setup.end()));
    }
    process.push_back(std::make_shared<TIfExpr>(loc,
        std::make_shared<TUnaryExpr>(loc, TOperator("!"), std::move(insertCall)),
        block({std::make_shared<TReturnExpr>(loc, number(0, boolType))}), nullptr));

    auto selected = binary("||", ident("selection_is_null"),
        binary("!=", std::make_shared<TIndexExpr>(loc, ident("selection"), ident("i")),
            number(0, u8Type)));
    auto loop = block({
        std::make_shared<TIfExpr>(loc, std::move(selected), block(std::move(process)), nullptr),
        assign("i", binary("+", ident("i"), numI64(1))),
    });
    body.push_back(std::make_shared<TWhileStmtExpr>(
        loc, binary("<", ident("i"), ident("n")), std::move(loop)));
    body.push_back(std::make_shared<TReturnExpr>(loc, number(1, boolType)));

    return std::make_shared<TFunDecl>(
        loc, funcName, std::move(params),
        std::make_shared<TBlockExpr>(loc, std::move(body)), boolType);
}

NQumir::NAst::TExprPtr GenJoinFinalizeSemiAntiAst(
    const TJoinKeyDescriptor& key,
    bool isAnti,
    const std::string& funcName,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType)
{
    NQumir::TLocation loc{};

    if (key.HasDistinctLookupType()) {
        throw NQumir::TError(
            "GenJoinFinalizeSemiAntiAst: string keys not supported yet");
    }

    auto i64Type = std::make_shared<TIntegerType>();
    auto boolType = std::make_shared<TBoolType>();
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
    auto index = [&](TExprPtr arr, TExprPtr idx) -> TExprPtr {
        return std::make_shared<TIndexExpr>(loc, std::move(arr), std::move(idx));
    };

    auto hashTableRefType = std::make_shared<TReferenceType>(
        std::make_shared<TNamedType>("HashTable", hashTableType));
    auto pairBufferRefType = std::make_shared<TReferenceType>(
        std::make_shared<TNamedType>("PairBuffer", pairBufferType));

    // Key pointer types for GroupKeys (own) and Keys (opp)
    auto ptrKeyType = std::make_shared<TPointerType>(key.KeyType);

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "own", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "opp", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "pairs", pairBufferRefType),
    };

    std::vector<TExprPtr> body;
    // own_gkeys = cast(own.GroupKeys) <ptr Key>
    body.push_back(var("own_gkeys", ptrKeyType));
    body.push_back(assign("own_gkeys", cast(field("own", "GroupKeys"), ptrKeyType)));
    // own_aggs = own.AggBuffers
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);
    body.push_back(var("own_aggs", ptrPtrI64Type));
    body.push_back(assign("own_aggs", field("own", "AggBuffers")));
    // own_counts = own_aggs[0], own_datas = own_aggs[2]
    body.push_back(var("own_counts", ptrI64Type));
    body.push_back(assign("own_counts", index(ident("own_aggs"), numI64(0))));
    body.push_back(var("own_datas", ptrI64Type));
    body.push_back(assign("own_datas", index(ident("own_aggs"), numI64(2))));
    // opp_keys = cast(opp.Keys) <ptr Key>
    body.push_back(var("opp_keys", ptrKeyType));
    body.push_back(assign("opp_keys", cast(field("opp", "Keys"), ptrKeyType)));
    // slot = 0
    body.push_back(var("slot", i64Type));
    body.push_back(assign("slot", numI64(0)));

    // Inner bucket drain loop body (built once, reused by reference in while)
    // while k < bcount: if !pb_push(pairs, bdata[k], -1): return #f; k++
    std::vector<TExprPtr> drainBody;
    {
        auto pushCall = call("pb_push", {
            ident("pairs"),
            index(ident("bdata"), ident("k")),
            numI64(-1),
        });
        drainBody.push_back(std::make_shared<TIfExpr>(loc,
            std::make_shared<TUnaryExpr>(loc, TOperator("!"), std::move(pushCall)),
            block({std::make_shared<TReturnExpr>(loc, number(0, boolType))}), nullptr));
        drainBody.push_back(assign("k", binary("+", ident("k"), numI64(1))));
    }

    // Outer slot loop body
    std::vector<TExprPtr> slotBody;
    {
        // key = own_gkeys[slot]
        slotBody.push_back(var("key", key.KeyType));
        slotBody.push_back(assign("key",
            index(ident("own_gkeys"), ident("slot"))));
        // opp_slot = rh_lookup_slot(opp_keys, opp.Dist, opp.SlotId, opp.Capacity, key)
        slotBody.push_back(var("opp_slot", i64Type));
        slotBody.push_back(assign("opp_slot",
            call("rh_lookup_slot", {
                ident("opp_keys"),
                field("opp", "Dist"),
                field("opp", "SlotId"),
                field("opp", "Capacity"),
                ident("key"),
            })));
        // found = opp_slot != -1
        slotBody.push_back(var("found", boolType));
        slotBody.push_back(assign("found",
            binary("!=", ident("opp_slot"), numI64(-1))));
        // emit_condition: SEMI → found; ANTI → !found
        TExprPtr emitCond = isAnti
            ? std::static_pointer_cast<TExpr>(
                  std::make_shared<TUnaryExpr>(loc, TOperator("!"), ident("found")))
            : ident("found");
        // if emit_condition: drain bucket
        std::vector<TExprPtr> emitBody;
        {
            emitBody.push_back(var("bcount", i64Type));
            emitBody.push_back(assign("bcount",
                index(ident("own_counts"), ident("slot"))));
            emitBody.push_back(var("bdata", ptrI64Type));
            emitBody.push_back(assign("bdata",
                cast(index(ident("own_datas"), ident("slot")), ptrI64Type)));
            emitBody.push_back(var("k", i64Type));
            emitBody.push_back(assign("k", numI64(0)));
            emitBody.push_back(std::make_shared<TWhileStmtExpr>(
                loc, binary("<", ident("k"), ident("bcount")),
                block(std::vector<TExprPtr>(drainBody))));
        }
        slotBody.push_back(std::make_shared<TIfExpr>(loc,
            std::move(emitCond), block(std::move(emitBody)), nullptr));
        slotBody.push_back(assign("slot", binary("+", ident("slot"), numI64(1))));
    }

    body.push_back(std::make_shared<TWhileStmtExpr>(
        loc, binary("<", ident("slot"), field("own", "Size")),
        block(std::move(slotBody))));
    body.push_back(std::make_shared<TReturnExpr>(loc, number(1, boolType)));

    return std::make_shared<TFunDecl>(
        loc, funcName, std::move(params),
        std::make_shared<TBlockExpr>(loc, std::move(body)), boolType);
}

namespace {

NQumir::NAst::TExprPtr GenJoinBatchAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType,
    bool insertRows)
{
    NQumir::TLocation loc{};

    if (key.HasDistinctLookupType()) {
        throw NQumir::TError(
            "GenJoinBatchAst: string keys are not supported yet (Stage 4 fixed-key scope)");
    }

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);

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
    auto rowSetPtrType = std::make_shared<TPointerType>(
        std::make_shared<TNamedType>("TRowSet", rowSetType));
    auto pairBufferRefType = std::make_shared<TReferenceType>(
        std::make_shared<TNamedType>("PairBuffer", pairBufferType));
    std::vector<TParam> params;
    if (insertRows) {
        params.push_back(std::make_shared<TVarStmt>(loc, "own", hashTableRefType));
        params.push_back(std::make_shared<TVarStmt>(loc, "opp", hashTableRefType));
    } else {
        params.push_back(std::make_shared<TVarStmt>(loc, "build", hashTableRefType));
    }
    params.push_back(std::make_shared<TVarStmt>(loc, "batch", rowSetRefType));
    params.push_back(std::make_shared<TVarStmt>(loc, "batch_idx", i64Type));
    params.push_back(std::make_shared<TVarStmt>(loc, "pairs", pairBufferRefType));
    params.push_back(std::make_shared<TVarStmt>(loc, "left_store", rowSetPtrType));
    params.push_back(std::make_shared<TVarStmt>(loc, "right_store", rowSetPtrType));

    auto ptrColumnType = [&]() -> TTypePtr {
        auto* rowSet = static_cast<TStructType*>(rowSetType.get());
        for (const auto& [name, type] : rowSet->Fields) {
            if (name == "Columns") {
                return type;
            }
        }
        return std::make_shared<TPointerType>(columnType);
    }();
    auto columnValueType = [&]() -> TTypePtr {
        if (auto pointer = TMaybeType<TPointerType>(ptrColumnType)) {
            return pointer.Cast()->PointeeType;
        }
        return columnType;
    }();
    auto columnAt = [&](int32_t index) -> TExprPtr {
        return std::make_shared<TIndexExpr>(loc, ident("cols"), numI64(index));
    };

    std::vector<TExprPtr> body;
    body.push_back(var("n", i64Type));
    body.push_back(assign("n", field("batch", "RowCount")));
    body.push_back(var("selection", ptrU8Type));
    body.push_back(assign("selection", field("batch", "Selection")));
    body.push_back(var("cols", ptrColumnType));
    body.push_back(assign("cols", field("batch", "Columns")));
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        const auto& keyField = key.Fields[fieldIndex];
        const int32_t columnIndex =
            isLeft ? keyField.LeftColumnIndex : keyField.RightColumnIndex;
        const std::string name = "key_column_" + std::to_string(fieldIndex);
        body.push_back(var(name, columnValueType));
        body.push_back(assign(name, columnAt(columnIndex)));
    }
    body.push_back(var("selection_is_null", boolType));
    body.push_back(assign("selection_is_null",
        binary("==", cast(ident("selection"), i64Type), numI64(0))));
    body.push_back(var("i", i64Type));
    body.push_back(assign("i", numI64(0)));

    // Per-row key materialization, reusing the shared column value builder.
    std::vector<TColumnValueAst> keyFields;
    keyFields.reserve(key.Fields.size());
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        auto logicalType = key.Fields[fieldIndex].IsNullable
            ? std::make_shared<TNullable>(key.Fields[fieldIndex].Type)
            : key.Fields[fieldIndex].Type;
        keyFields.push_back(BuildColumnValueAst(
            "key_column_" + std::to_string(fieldIndex), "i",
            "key_value_" + std::to_string(fieldIndex),
            std::move(logicalType), /*stringViewType=*/nullptr));
    }

    // Build the <named Key> struct value (key_N <- Value, valid_N <- IsValid,
    // padding <- 0), in the order the Key struct declares.
    auto namedKey = TMaybeType<TNamedType>(key.KeyType);
    auto keyStruct = namedKey
        ? TMaybeType<TStructType>(namedKey.Cast()->UnderlyingType)
        : TMaybeType<TStructType>(key.KeyType);
    if (!keyStruct) {
        throw NQumir::TError("GenJoinBatchAst: key must be a struct");
    }
    std::vector<TExprPtr> structFields;
    structFields.reserve(keyStruct.Cast()->Fields.size());
    for (const auto& [fieldName, fieldType] : keyStruct.Cast()->Fields) {
        if (fieldName.starts_with("__qdb_padding_")) {
            structFields.push_back(number(0, fieldType));
            continue;
        }
        const bool validity = fieldName.starts_with("valid_");
        const std::string_view prefix = validity ? "valid_" : "key_";
        const size_t fieldIndex = std::stoull(fieldName.substr(prefix.size()));
        structFields.push_back(validity
            ? keyFields[fieldIndex].IsValid
            : keyFields[fieldIndex].Value);
    }
    TExprPtr keyValue = std::make_shared<TStructConstructExpr>(
        loc, key.KeyType, std::move(structFields));

    // own_row_id = (batch_idx << 32) | (i & 0xffffffff)
    auto ownRowId = binary("+",
        binary("<<", ident("batch_idx"), numI64(32)),
        binary("&", ident("i"), numI64(0xffffffff)));

    TExprPtr emitCall;
    if (insertRows) {
        emitCall = call("jt_emit_and_insert", {
            ident("own"), ident("opp"), std::move(keyValue), std::move(ownRowId),
            numI64(isLeft ? 1 : 0), ident("pairs"),
            ident("left_store"), ident("right_store"),
        });
    } else {
        auto storeBatchRef = [&](const char* store) -> TExprPtr {
            return std::make_shared<TIndexExpr>(loc, ident(store), numI64(0));
        };
        emitCall = call("jt_probe_and_emit", {
            ident("build"), std::move(keyValue), std::move(ownRowId),
            numI64(isLeft ? 1 : 0), ident("pairs"),
            ident("left_store"), ident("right_store"),
            isLeft ? ident("batch") : storeBatchRef("left_store"),
            isLeft ? storeBatchRef("right_store") : ident("batch"),
        });
    }

    std::vector<TExprPtr> process;
    for (auto& keyField : keyFields) {
        process.insert(process.end(),
            std::make_move_iterator(keyField.Setup.begin()),
            std::make_move_iterator(keyField.Setup.end()));
    }
    // if (!jt_emit_and_insert(...)) return false;
    process.push_back(std::make_shared<TIfExpr>(loc,
        std::make_shared<TUnaryExpr>(loc, TOperator("!"), std::move(emitCall)),
        block({std::make_shared<TReturnExpr>(loc, number(0, boolType))}), nullptr));

    auto selected = binary("||", ident("selection_is_null"),
        binary("!=", std::make_shared<TIndexExpr>(loc, ident("selection"), ident("i")),
            number(0, u8Type)));
    auto loop = block({
        std::make_shared<TIfExpr>(loc, std::move(selected), block(std::move(process)), nullptr),
        assign("i", binary("+", ident("i"), numI64(1))),
    });
    body.push_back(std::make_shared<TWhileStmtExpr>(
        loc, binary("<", ident("i"), ident("n")), std::move(loop)));
    body.push_back(std::make_shared<TReturnExpr>(loc, number(1, boolType)));

    auto function = std::make_shared<TFunDecl>(
        loc, funcName, std::move(params),
        std::make_shared<TBlockExpr>(loc, std::move(body)), boolType);
    return function;
}

} // namespace

NQumir::NAst::TExprPtr GenJoinProcessAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType)
{
    return GenJoinBatchAst(key, isLeft, funcName, std::move(columnType),
        std::move(rowSetType), std::move(hashTableType), std::move(pairBufferType),
        /*insertRows=*/true);
}

NQumir::NAst::TExprPtr GenJoinProbeAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType)
{
    return GenJoinBatchAst(key, isLeft, funcName, std::move(columnType),
        std::move(rowSetType), std::move(hashTableType), std::move(pairBufferType),
        /*insertRows=*/false);
}

} // namespace NQdb::NKernel
