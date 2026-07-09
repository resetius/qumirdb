#include <qdb/kernel/join_gen.h>

#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/builder.h>
#include <qdb/kernel/column_value.h>
#include <qdb/kernel/gen.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>

#include <algorithm>

namespace NQdb::NKernel {

using namespace NQumir::NAst;

namespace {

TTypePtr NamedType(const std::string& name) {
    return std::make_shared<TNamedType>(name, nullptr);
}

TTypePtr ColumnPointerType(const TTypePtr& columnType, const TTypePtr& rowSetType) {
    auto rowSetStruct = TMaybeType<TStructType>(UnwrapNamedType(rowSetType));
    if (rowSetStruct) {
        for (const auto& [name, type] : rowSetStruct.Cast()->Fields) {
            if (name == "Columns") {
                return type;
            }
        }
    }
    return std::make_shared<TPointerType>(
        columnType ? columnType : NamedType("TColumn"));
}

TTypePtr PointerPointeeOr(const TTypePtr& pointerType, const TTypePtr& fallback) {
    if (auto pointer = TMaybeType<TPointerType>(pointerType)) {
        return pointer.Cast()->PointeeType;
    }
    return fallback ? fallback : NamedType("TColumn");
}

} // namespace

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
        AsNamed("HashTable", hashTableType));
    auto rowSetRefType = std::make_shared<TReferenceType>(
        AsNamed("TRowSet", rowSetType));
    auto pairBufferRefType = std::make_shared<TReferenceType>(
        AsNamed("PairBuffer", pairBufferType));

    // Same ABI as GenJoinProcessAst: (own, opp, batch, batch_idx, pairs) -> bool
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "own", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "opp", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "batch_idx", i64Type),
        std::make_shared<TVarStmt>(loc, "pairs", pairBufferRefType),
    };

    auto ptrColumnType = ColumnPointerType(columnType, rowSetType);
    auto columnValueType = PointerPointeeOr(ptrColumnType, columnType);
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
        AsNamed("HashTable", hashTableType));
    auto pairBufferRefType = std::make_shared<TReferenceType>(
        AsNamed("PairBuffer", pairBufferType));

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
        AsNamed("HashTable", hashTableType));
    auto rowSetRefType = std::make_shared<TReferenceType>(
        AsNamed("TRowSet", rowSetType));
    auto rowSetPtrType = std::make_shared<TPointerType>(
        AsNamed("TRowSet", rowSetType));
    auto pairBufferRefType = std::make_shared<TReferenceType>(
        AsNamed("PairBuffer", pairBufferType));
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

    auto ptrColumnType = ColumnPointerType(columnType, rowSetType);
    auto columnValueType = PointerPointeeOr(ptrColumnType, columnType);
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

NQumir::NAst::TExprPtr GenJoinDispatchAst(
    int64_t keySize,
    EJoinType type,
    bool hasResidual,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType)
{
    namespace Oz = NKernel::NOz;

    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto hashTableRefType = std::make_shared<TReferenceType>(
        AsNamed("HashTable", hashTableType));
    auto rowSetRefType = std::make_shared<TReferenceType>(
        AsNamed("TRowSet", rowSetType));
    auto rowSetPtrType = std::make_shared<TPointerType>(
        AsNamed("TRowSet", rowSetType));
    auto pairBufferRefType = std::make_shared<TReferenceType>(
        AsNamed("PairBuffer", pairBufferType));

    auto number = [&](int64_t value) -> TExprPtr {
        return Oz::TypedInt(value, i64Type);
    };
    auto opIs = [&](int64_t op) -> TExprPtr {
        return Oz::Bin(TOperator("=="), Oz::Ident("op"), number(op));
    };
    auto boolReturn = [&](bool value) -> TExprPtr {
        return Oz::Return(Oz::Bool(value));
    };

    auto processLeft = [&]() {
        return Oz::Call("jt_process_left", {
            Oz::Ident("left"),
            Oz::Ident("right"),
            Oz::Ident("batch"),
            Oz::Ident("batch_idx"),
            Oz::Ident("pairs"),
            Oz::Ident("left_store"),
            Oz::Ident("right_store"),
        });
    };
    auto processRight = [&]() {
        return Oz::Call("jt_process_right", {
            Oz::Ident("right"),
            Oz::Ident("left"),
            Oz::Ident("batch"),
            Oz::Ident("batch_idx"),
            Oz::Ident("pairs"),
            Oz::Ident("left_store"),
            Oz::Ident("right_store"),
        });
    };
    auto streamLeft = [&]() {
        return Oz::Call("jt_probe_left_stream", {
            Oz::Ident("right"),
            Oz::Ident("batch"),
            Oz::Ident("batch_idx"),
            Oz::Ident("pairs"),
            Oz::Ident("left_store"),
            Oz::Ident("right_store"),
        });
    };
    auto streamRight = [&]() {
        return Oz::Call("jt_probe_right_stream", {
            Oz::Ident("left"),
            Oz::Ident("batch"),
            Oz::Ident("batch_idx"),
            Oz::Ident("pairs"),
            Oz::Ident("left_store"),
            Oz::Ident("right_store"),
        });
    };
    auto insertRightKeyOnly = [&]() {
        return Oz::Call("jt_insert_key_only", {
            Oz::Ident("right"),
            Oz::Ident("left"),
            Oz::Ident("batch"),
            Oz::Ident("batch_idx"),
            Oz::Ident("pairs"),
        });
    };

    auto updateRight = [&]() -> TExprPtr {
        if ((type == EJoinType::LeftSemi || type == EJoinType::LeftAnti) &&
            !hasResidual) {
            return insertRightKeyOnly();
        }
        if ((type == EJoinType::LeftSemi || type == EJoinType::LeftAnti) &&
            hasResidual) {
            return streamRight();
        }
        return processRight();
    };

    auto rightOuterFinalize = [&]() -> TExprPtr {
        return Oz::Block({
            Oz::Var("ok", boolType),
            Oz::Assign("ok", Oz::Call("jt_finalize_outer", {
                Oz::Ident("right"),
                Oz::Ident("left"),
                Oz::Ident("pairs"),
            })),
            Oz::If(
                Oz::Unary(TOperator("!"), Oz::Ident("ok")),
                Oz::Block({boolReturn(false)})),
            Oz::Var("data", ptrI64Type),
            Oz::Assign("data", Oz::Field("pairs", "Data")),
            Oz::Var("i", i64Type),
            Oz::Assign("i", number(0)),
            Oz::Var("tmp", i64Type),
            Oz::While(
                Oz::Bin(TOperator("<"), Oz::Ident("i"), Oz::Field("pairs", "Count")),
                Oz::Block({
                    Oz::Assign("tmp",
                        Oz::Index("data", Oz::Mul(Oz::Ident("i"), number(2)))),
                    Oz::ArrayAssign("data",
                        Oz::Mul(Oz::Ident("i"), number(2)),
                        Oz::Index("data", Oz::Add(
                            Oz::Mul(Oz::Ident("i"), number(2)), number(1)))),
                    Oz::ArrayAssign("data",
                        Oz::Add(Oz::Mul(Oz::Ident("i"), number(2)), number(1)),
                        Oz::Ident("tmp")),
                    Oz::Assign("i", Oz::Add(Oz::Ident("i"), number(1))),
                })),
            boolReturn(true),
        });
    };

    auto finalize = [&]() -> TExprPtr {
        if ((type == EJoinType::LeftSemi || type == EJoinType::LeftAnti) &&
            !hasResidual) {
            return Oz::Return(Oz::Call("jt_finalize_semi_anti", {
                Oz::Ident("left"),
                Oz::Ident("right"),
                Oz::Ident("pairs"),
            }));
        }
        if (type == EJoinType::Left) {
            return Oz::Return(Oz::Call("jt_finalize_outer", {
                Oz::Ident("left"),
                Oz::Ident("right"),
                Oz::Ident("pairs"),
            }));
        }
        if (type == EJoinType::Right) {
            return rightOuterFinalize();
        }
        return boolReturn(true);
    };

    Oz::TFunBuilder builder("jt_dispatch");
    builder
        .Param("left", hashTableRefType)
        .Param("right", hashTableRefType)
        .Param("batch", rowSetRefType)
        .Param("batch_idx", i64Type)
        .Param("pairs", pairBufferRefType)
        .Param("left_store", rowSetPtrType)
        .Param("right_store", rowSetPtrType)
        .Param("arg", i64Type)
        .Param("op", i64Type)
        .Return(boolType)
        .Stmt(Oz::If(opIs(0),
            Oz::Block({
                Oz::Return(Oz::Bin(TOperator("&&"),
                    Oz::Call("jt_init", {
                        Oz::Ident("left"), Oz::Ident("arg"), number(keySize)}),
                    Oz::Call("jt_init", {
                        Oz::Ident("right"), Oz::Ident("arg"), number(keySize)}))),
            })))
        .Stmt(Oz::If(opIs(1), Oz::Block({Oz::Return(processLeft())})))
        .Stmt(Oz::If(opIs(2), Oz::Block({Oz::Return(updateRight())})))
        .Stmt(Oz::If(opIs(3), Oz::Block({Oz::Return(streamLeft())})))
        .Stmt(Oz::If(opIs(4), Oz::Block({Oz::Return(streamRight())})))
        .Stmt(Oz::If(opIs(5), finalize()))
        .Stmt(Oz::If(opIs(6),
            Oz::Block({
                Oz::Call("jt_destroy", {Oz::Ident("left")}),
                Oz::Call("jt_destroy", {Oz::Ident("right")}),
                Oz::Call("pb_destroy", {Oz::Ident("pairs")}),
                boolReturn(true),
            })))
        .Stmt(boolReturn(false));

    return std::move(builder).Build();
}

NQumir::NAst::TExprPtr GenJoinHashBatchAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType)
{
    NQumir::TLocation loc{};

    if (key.HasDistinctLookupType()) {
        throw NQumir::TError(
            "GenJoinHashBatchAst: string keys are not supported yet");
    }

    auto i64Type = std::make_shared<TIntegerType>();
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrU64Type = std::make_shared<TPointerType>(u64Type);

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
    auto cast = [&](TExprPtr expr, TTypePtr type) -> TExprPtr {
        return std::make_shared<TCastExpr>(loc, std::move(expr), std::move(type));
    };
    auto call = [&](const std::string& name, std::vector<TExprPtr> args) -> TExprPtr {
        return std::make_shared<TCallExpr>(loc, ident(name), std::move(args));
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

    auto rowSetRefType = std::make_shared<TReferenceType>(
        AsNamed("TRowSet", rowSetType));
    std::vector<TParam> params{
        std::make_shared<TVarStmt>(loc, "batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "hashes", ptrU64Type),
    };

    auto ptrColumnType = ColumnPointerType(columnType, rowSetType);
    auto columnValueType = PointerPointeeOr(ptrColumnType, columnType);
    auto columnAt = [&](int32_t index) -> TExprPtr {
        return std::make_shared<TIndexExpr>(loc, ident("cols"), numI64(index));
    };

    std::vector<TExprPtr> body;
    body.push_back(var("n", i64Type));
    body.push_back(assign("n", field("batch", "RowCount")));
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
    body.push_back(var("i", i64Type));
    body.push_back(assign("i", numI64(0)));

    std::vector<TColumnValueAst> keyFields;
    keyFields.reserve(key.Fields.size());
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        auto logicalType = key.Fields[fieldIndex].IsNullable
            ? std::make_shared<TNullable>(key.Fields[fieldIndex].Type)
            : key.Fields[fieldIndex].Type;
        keyFields.push_back(BuildColumnValueAst(
            "key_column_" + std::to_string(fieldIndex), "i",
            "hash_key_value_" + std::to_string(fieldIndex),
            std::move(logicalType), /*stringViewType=*/nullptr));
    }

    auto namedKey = TMaybeType<TNamedType>(key.KeyType);
    auto keyStruct = namedKey
        ? TMaybeType<TStructType>(namedKey.Cast()->UnderlyingType)
        : TMaybeType<TStructType>(key.KeyType);
    if (!keyStruct) {
        throw NQumir::TError("GenJoinHashBatchAst: key must be a struct");
    }
    std::vector<TExprPtr> structFields;
    structFields.reserve(keyStruct.Cast()->Fields.size());
    for (const auto& [fieldName, fieldType] : keyStruct.Cast()->Fields) {
        const bool validity = fieldName.starts_with("valid_");
        const std::string_view prefix = validity ? "valid_" : "key_";
        const size_t fieldIndex = std::stoull(fieldName.substr(prefix.size()));
        structFields.push_back(validity
            ? keyFields[fieldIndex].IsValid
            : keyFields[fieldIndex].Value);
    }
    TExprPtr keyValue = std::make_shared<TStructConstructExpr>(
        loc, key.KeyType, std::move(structFields));

    std::vector<TExprPtr> process;
    for (auto& keyField : keyFields) {
        process.insert(process.end(),
            std::make_move_iterator(keyField.Setup.begin()),
            std::make_move_iterator(keyField.Setup.end()));
    }
    process.push_back(std::make_shared<TArrayAssignExpr>(
        loc, "hashes", std::vector<TExprPtr>{ident("i")},
        cast(call("rh_hash", {std::move(keyValue)}), u64Type)));
    process.push_back(assign("i", binary("+", ident("i"), numI64(1))));

    body.push_back(std::make_shared<TWhileStmtExpr>(
        loc, binary("<", ident("i"), ident("n")), block(std::move(process))));
    body.push_back(std::make_shared<TReturnExpr>(loc, number(1, boolType)));

    return std::make_shared<TFunDecl>(
        loc, funcName, std::move(params),
        std::make_shared<TBlockExpr>(loc, std::move(body)), boolType);
}

namespace {

// One output column of the generated materializer: which side it reads,
// the source column index on that side, and its logical type.
struct TJoinOutputColumn {
    bool IsLeft = true;
    int32_t SrcColIdx = 0;
    NQumir::NAst::TTypePtr Type;
    bool IsString = false;
    int64_t FixedWidth = 0; // bytes; 0 for strings
};

std::vector<TJoinOutputColumn> BuildJoinOutputColumns(
    const TStructType& leftType,
    const TStructType& rightType,
    bool includeRight)
{
    auto classify = [](const TTypePtr& type, TJoinOutputColumn& col) {
        auto inner = UnwrapNamedType(UnwrapNullableType(type));
        if (auto integer = TMaybeType<TIntegerType>(inner)) {
            col.FixedWidth = integer.Cast()->BitWidth() / 8;
        } else if (TMaybeType<TFloatType>(inner)) {
            col.FixedWidth = 8;
        } else if (TMaybeType<TStringType>(inner)) {
            col.IsString = true;
        } else {
            throw NQumir::TError(
                "GenJoinMaterializeAst: cannot materialize column of type " +
                (type ? type->ToString() : std::string("<null>")));
        }
    };

    std::vector<TJoinOutputColumn> columns;
    for (int32_t i = 0; i < static_cast<int32_t>(leftType.Fields.size()); ++i) {
        TJoinOutputColumn col{.IsLeft = true, .SrcColIdx = i,
            .Type = leftType.Fields[i].second};
        classify(col.Type, col);
        columns.push_back(std::move(col));
    }
    if (includeRight) {
        for (int32_t i = 0; i < static_cast<int32_t>(rightType.Fields.size()); ++i) {
            TJoinOutputColumn col{.IsLeft = false, .SrcColIdx = i,
                .Type = rightType.Fields[i].second};
            classify(col.Type, col);
            columns.push_back(std::move(col));
        }
    }
    return columns;
}

} // namespace

NQumir::NAst::TExprPtr GenJoinMaterializeAst(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    bool includeRight,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType)
{
    namespace Oz = NKernel::NOz;
    NQumir::TLocation loc{};

    constexpr int64_t kColumnSize = 48; // sizeof(TColumn), see modules/qumirdb.cpp
    constexpr int64_t kPtrSize = 8;

    const auto columns = BuildJoinOutputColumns(leftType, rightType, includeRight);
    const int64_t columnCount = static_cast<int64_t>(columns.size());
    const bool anyString = std::any_of(columns.begin(), columns.end(),
        [](const TJoinOutputColumn& c) { return c.IsString; });
    const bool anyRight = std::any_of(columns.begin(), columns.end(),
        [](const TJoinOutputColumn& c) { return !c.IsLeft; });

    // owners: columns array + per column Data + Mask (+ Offsets for strings).
    int64_t ownedPtrCount = 1;
    for (const auto& col : columns) {
        ownedPtrCount += col.IsString ? 3 : 2;
    }

    auto i8Type = std::make_shared<TIntegerType>(TIntegerType::I8);
    auto i32Type = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrI8Type = std::make_shared<TPointerType>(i8Type);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);

    auto ptrColumnType = ColumnPointerType(columnType, rowSetType);
    auto columnValueType = PointerPointeeOr(ptrColumnType, columnType);
    auto rowSetRefType = std::make_shared<TReferenceType>(
        AsNamed("TRowSet", rowSetType));
    auto rowSetPtrType = std::make_shared<TPointerType>(
        AsNamed("TRowSet", rowSetType));
    auto pairBufferRefType = std::make_shared<TReferenceType>(
        AsNamed("PairBuffer", pairBufferType));

    auto number = [&](int64_t value) -> TExprPtr {
        return Oz::TypedInt(value, i64Type);
    };
    auto allocAs = [&](TTypePtr type, TExprPtr byteSize) -> TExprPtr {
        return Oz::Cast(
            Oz::Call("qdb_alloc", {std::move(byteSize)}), std::move(type));
    };
    auto column = [&](size_t idx) -> TExprPtr {
        return Oz::Index("columns", number(static_cast<int64_t>(idx)));
    };

    Oz::TFunBuilder builder("jt_materialize");
    builder
        .Param("pairs", pairBufferRefType)
        .Param("left_store", rowSetPtrType)
        .Param("right_store", rowSetPtrType)
        .Param("stream_left", rowSetRefType)
        .Param("stream_right", rowSetRefType)
        .Param("start", i64Type)
        .Param("limit", i64Type)
        .Param("out", rowSetRefType)
        .Return(i64Type)
        .Var("n", i64Type)
        .Assign("n", Oz::Sub(Oz::Field("pairs", "Count"), Oz::Ident("start")))
        .Stmt(Oz::If(
            Oz::Bin(TOperator("<"), Oz::Ident("limit"), Oz::Ident("n")),
            Oz::Block({Oz::Assign("n", Oz::Ident("limit"))})))
        .Stmt(Oz::If(
            Oz::Bin(TOperator("<="), Oz::Ident("n"), number(0)),
            Oz::Block({Oz::Return(number(0))})))
        .Var("pair_data", ptrI64Type)
        .Assign("pair_data", Oz::Field("pairs", "Data"))
        .Var("mask_bytes", i64Type)
        .Assign("mask_bytes",
            Oz::Bin(TOperator(">>"),
                Oz::Add(Oz::Ident("n"), number(7)), number(3)))
        .Var("owners", ptrI64Type)
        .Assign("owners", allocAs(ptrI64Type,
            number((ownedPtrCount + 1) * kPtrSize)))
        .Stmt(Oz::ArrayAssign("owners", number(0), number(ownedPtrCount)))
        .Var("owner_idx", i64Type)
        .Assign("owner_idx", number(1));

    auto remember = [&](const std::string& ptrName) {
        builder
            .Stmt(Oz::ArrayAssign("owners", Oz::Ident("owner_idx"),
                Oz::Cast(Oz::Ident(ptrName), i64Type)))
            .Assign("owner_idx",
                Oz::Add(Oz::Ident("owner_idx"), number(1)));
    };

    builder
        .Var("columns", ptrColumnType)
        .Assign("columns", allocAs(ptrColumnType,
            number(std::max<int64_t>(columnCount, 1) * kColumnSize)));
    remember("columns");

    // Allocation phase: fixed-width Data now, string Data after the size pass.
    for (size_t c = 0; c < columns.size(); ++c) {
        const auto& col = columns[c];
        const std::string dataName = "col_data_" + std::to_string(c);
        const std::string maskName = "col_mask_" + std::to_string(c);
        const std::string offsetsName = "col_offsets_" + std::to_string(c);

        if (col.IsString) {
            builder
                .Var(offsetsName, ptrI64Type)
                .Assign(offsetsName, allocAs(ptrI64Type,
                    Oz::Mul(Oz::Add(Oz::Ident("n"), number(1)), number(8))));
            remember(offsetsName);
            builder
                .Stmt(Oz::ArrayAssign(offsetsName, number(0), number(0)))
                .Stmt(Oz::FieldAssign(column(c), "Offsets", Oz::Ident(offsetsName)))
                .Stmt(Oz::FieldAssign(column(c), "OffsetWidth",
                    Oz::TypedInt(8, u8Type)));
        } else {
            builder
                .Var(dataName, ptrU8Type)
                .Assign(dataName, allocAs(ptrU8Type,
                    Oz::Mul(Oz::Ident("n"), number(col.FixedWidth))));
            remember(dataName);
            builder
                .Stmt(Oz::FieldAssign(column(c), "Data",
                    Oz::Cast(Oz::Ident(dataName), ptrI8Type)))
                .Stmt(Oz::FieldAssign(column(c), "Offsets", Oz::NullPtr(ptrI64Type)))
                .Stmt(Oz::FieldAssign(column(c), "OffsetWidth",
                    Oz::TypedInt(0, u8Type)));
        }
        builder
            .Var(maskName, ptrU8Type)
            .Assign(maskName, allocAs(ptrU8Type, Oz::Ident("mask_bytes")));
        remember(maskName);
        builder
            .Stmt(Oz::FieldAssign(column(c), "Mask", Oz::Ident(maskName)))
            .Stmt(Oz::FieldAssign(column(c), "MaskBitOffset", Oz::TypedInt(0, i32Type)))
            .Stmt(Oz::FieldAssign(column(c), "DataBitOffset", Oz::TypedInt(0, i32Type)));
    }

    // Per-row decode of one side's packed row id. Emits, into `stmts`:
    //   <side>_id, <side>_null, <side>_row, <side>_cols.
    // The null-padding check (id == -1) must precede the batch decode:
    // -1 >> 32 == -1 would collide with the stream-batch encoding.
    auto decodeSide = [&](std::vector<TExprPtr>& stmts, const std::string& side,
        int64_t pairOffset, const char* storeName, const char* streamName)
    {
        const std::string idName = side + "_id";
        const std::string nullName = side + "_null";
        const std::string rowName = side + "_row";
        const std::string batchName = side + "_batch";
        const std::string colsName = side + "_cols";

        auto pairIndex = Oz::Add(
            Oz::Mul(Oz::Add(Oz::Ident("start"), Oz::Ident("j")), number(2)),
            number(pairOffset));
        stmts.push_back(Oz::Var(idName, i64Type));
        stmts.push_back(Oz::Assign(idName,
            Oz::Index("pair_data", std::move(pairIndex))));
        stmts.push_back(Oz::Var(nullName, boolType));
        stmts.push_back(Oz::Assign(nullName,
            Oz::Bin(TOperator("=="), Oz::Ident(idName), number(-1))));
        stmts.push_back(Oz::Var(rowName, i64Type));
        stmts.push_back(Oz::Assign(rowName, number(0)));
        stmts.push_back(Oz::Var(colsName, ptrColumnType));
        stmts.push_back(Oz::Assign(colsName, Oz::NullPtr(ptrColumnType)));

        auto storeColumns = std::make_shared<TFieldAccessExpr>(loc,
            std::make_shared<TIndexExpr>(loc,
                Oz::Ident(storeName), Oz::Ident(batchName)),
            "Columns");
        stmts.push_back(Oz::If(
            Oz::Unary(TOperator("!"), Oz::Ident(nullName)),
            Oz::Block({
                Oz::Var(batchName, i64Type),
                Oz::Assign(batchName,
                    Oz::Bin(TOperator(">>"), Oz::Ident(idName), number(32))),
                Oz::Assign(rowName,
                    Oz::Bin(TOperator("&"), Oz::Ident(idName), number(0xffffffff))),
                Oz::If(
                    Oz::Bin(TOperator("=="), Oz::Ident(batchName), number(-1)),
                    Oz::Block({Oz::Assign(colsName,
                        Oz::Field(streamName, "Columns"))}),
                    Oz::Block({Oz::Assign(colsName, storeColumns)})),
            })));
    };

    // Per-row source-column materialization shared by both passes. Wrapping the
    // logical type as nullable makes BuildColumnValueAst derive validity from
    // the source Mask (mask == null -> valid), so a non-nullable source column
    // costs one dead check instead of a separate code path.
    auto materializeSource = [&](std::vector<TExprPtr>& stmts, size_t c,
        const std::string& prefix) -> TColumnValueAst
    {
        const auto& col = columns[c];
        const std::string side = col.IsLeft ? "left" : "right";
        const std::string srcName = prefix + "_src";

        stmts.push_back(Oz::Var(srcName, columnValueType));
        stmts.push_back(Oz::Assign(srcName,
            Oz::Index(side + "_cols", number(col.SrcColIdx))));

        auto logicalType = IsNullableType(col.Type)
            ? col.Type
            : std::make_shared<TNullable>(col.Type);
        auto value = BuildColumnValueAst(
            srcName, side + "_row", prefix, logicalType, stringViewType);
        stmts.insert(stmts.end(),
            std::make_move_iterator(value.Setup.begin()),
            std::make_move_iterator(value.Setup.end()));
        return value;
    };

    // Pass A (strings only): accumulate payload sizes into Offsets.
    if (anyString) {
        std::vector<TExprPtr> loop;
        decodeSide(loop, "left", 0, "left_store", "stream_left");
        if (anyRight) {
            decodeSide(loop, "right", 1, "right_store", "stream_right");
        }
        for (size_t c = 0; c < columns.size(); ++c) {
            const auto& col = columns[c];
            if (!col.IsString) {
                continue;
            }
            const std::string side = col.IsLeft ? "left" : "right";
            const std::string lenName = "len_" + std::to_string(c);
            const std::string offsetsName = "col_offsets_" + std::to_string(c);

            loop.push_back(Oz::Var(lenName, i64Type));
            loop.push_back(Oz::Assign(lenName, number(0)));
            std::vector<TExprPtr> read;
            auto value = materializeSource(read, c, "sz_" + std::to_string(c));
            // The nullable string path yields a zero StringView when invalid,
            // so Size is already 0 for source nulls.
            read.push_back(Oz::Assign(lenName,
                std::make_shared<TFieldAccessExpr>(loc, value.Value, "Size")));
            loop.push_back(Oz::If(
                Oz::Unary(TOperator("!"), Oz::Ident(side + "_null")),
                Oz::Block(std::move(read))));
            loop.push_back(Oz::ArrayAssign(offsetsName,
                Oz::Add(Oz::Ident("j"), number(1)),
                Oz::Add(
                    Oz::Index(offsetsName, Oz::Ident("j")),
                    Oz::Ident(lenName))));
        }
        loop.push_back(Oz::Assign("j", Oz::Add(Oz::Ident("j"), number(1))));

        builder
            .Var("j", i64Type)
            .Assign("j", number(0))
            .Stmt(Oz::While(
                Oz::Bin(TOperator("<"), Oz::Ident("j"), Oz::Ident("n")),
                Oz::Block(std::move(loop))));

        // Now the payload sizes are known: allocate string Data buffers.
        for (size_t c = 0; c < columns.size(); ++c) {
            if (!columns[c].IsString) {
                continue;
            }
            const std::string dataName = "col_data_" + std::to_string(c);
            const std::string offsetsName = "col_offsets_" + std::to_string(c);
            builder
                .Var(dataName, ptrU8Type)
                .Assign(dataName, allocAs(ptrU8Type,
                    Oz::Index(offsetsName, Oz::Ident("n"))));
            remember(dataName);
            builder.Stmt(Oz::FieldAssign(column(c), "Data",
                Oz::Cast(Oz::Ident(dataName), ptrI8Type)));
        }
    }

    // Pass B: write values, copy string payloads, set validity bits.
    {
        std::vector<TExprPtr> loop;
        decodeSide(loop, "left", 0, "left_store", "stream_left");
        if (anyRight) {
            decodeSide(loop, "right", 1, "right_store", "stream_right");
        }
        for (size_t c = 0; c < columns.size(); ++c) {
            const auto& col = columns[c];
            const std::string side = col.IsLeft ? "left" : "right";
            const std::string validName = "out_valid_" + std::to_string(c);
            const std::string dataName = "col_data_" + std::to_string(c);
            const std::string maskName = "col_mask_" + std::to_string(c);
            const std::string prefix = "mat_" + std::to_string(c);

            loop.push_back(Oz::Var(validName, boolType));
            loop.push_back(Oz::Assign(validName, Oz::Bool(false)));

            if (col.IsString) {
                const std::string offsetsName = "col_offsets_" + std::to_string(c);
                const std::string svName = prefix + "_sv";
                const std::string copyName = prefix + "_copied";
                std::vector<TExprPtr> read;
                auto value = materializeSource(read, c, prefix);
                read.push_back(Oz::Assign(validName, value.IsValid));
                read.push_back(Oz::Var(svName, value.ValueType));
                read.push_back(Oz::Assign(svName, value.Value));
                auto destination = Oz::Cast(
                    Oz::Add(
                        Oz::Cast(Oz::Ident(dataName), i64Type),
                        Oz::Index(offsetsName, Oz::Ident("j"))),
                    ptrU8Type);
                read.push_back(Oz::Var(copyName, i64Type));
                read.push_back(Oz::Assign(copyName,
                    Oz::Call("qdb_string_copy_bytes", {
                        std::move(destination),
                        Oz::Field(svName, "Data"),
                        Oz::Field(svName, "Size")})));
                loop.push_back(Oz::If(
                    Oz::Unary(TOperator("!"), Oz::Ident(side + "_null")),
                    Oz::Block(std::move(read))));
            } else {
                const std::string valueName = prefix + "_out";
                const std::string typedDataName = prefix + "_data";
                std::vector<TExprPtr> read;
                auto value = materializeSource(read, c, prefix);
                read.push_back(Oz::Assign(validName, value.IsValid));
                read.push_back(Oz::Assign(valueName, value.Value));
                loop.push_back(Oz::Var(valueName, value.ValueType));
                loop.push_back(Oz::Assign(valueName,
                    Oz::Cast(number(0), value.ValueType)));
                loop.push_back(Oz::If(
                    Oz::Unary(TOperator("!"), Oz::Ident(side + "_null")),
                    Oz::Block(std::move(read))));
                auto typedPtr = std::make_shared<TPointerType>(value.ValueType);
                loop.push_back(Oz::Var(typedDataName, typedPtr));
                loop.push_back(Oz::Assign(typedDataName,
                    Oz::Cast(Oz::Ident(dataName), typedPtr)));
                loop.push_back(Oz::ArrayAssign(typedDataName,
                    Oz::Ident("j"), Oz::Ident(valueName)));
            }
            loop.push_back(Oz::Call("qdb_bitmap_set_valid", {
                Oz::Ident(maskName), Oz::Ident("j"), Oz::Ident(validName)}));
        }
        loop.push_back(Oz::Assign("j", Oz::Add(Oz::Ident("j"), number(1))));

        if (anyString) {
            builder.Assign("j", number(0));
        } else {
            builder.Var("j", i64Type).Assign("j", number(0));
        }
        builder.Stmt(Oz::While(
            Oz::Bin(TOperator("<"), Oz::Ident("j"), Oz::Ident("n")),
            Oz::Block(std::move(loop))));
    }

    builder
        .Stmt(Oz::FieldAssign(Oz::Ident("out"), "Columns", Oz::Ident("columns")))
        .Stmt(Oz::FieldAssign(Oz::Ident("out"), "ColumnCount", number(columnCount)))
        .Stmt(Oz::FieldAssign(Oz::Ident("out"), "RowCount", Oz::Ident("n")))
        .Stmt(Oz::FieldAssign(Oz::Ident("out"), "Selection", Oz::NullPtr(ptrU8Type)))
        .Stmt(Oz::FieldAssign(Oz::Ident("out"), "Destroy", Oz::NullPtr(ptrI64Type)))
        .Stmt(Oz::FieldAssign(Oz::Ident("out"), "Private", Oz::Ident("owners")))
        .Stmt(Oz::FieldAssign(Oz::Ident("out"), "RefCount", number(1)))
        .Stmt(Oz::Return(Oz::Ident("n")));

    return std::move(builder).Build();
}

} // namespace NQdb::NKernel
