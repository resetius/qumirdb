#include <qdb/kernel/join_gen.h>

#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/column_value.h>
#include <qdb/kernel/gen.h>
#include <qdb/types/nullable.h>

#include <qumir/error.h>

namespace NQqb::NKernel {

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

NQumir::NAst::TExprPtr GenJoinProcessAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType)
{
    NQumir::TLocation loc{};

    if (key.HasDistinctLookupType()) {
        throw NQumir::TError(
            "GenJoinProcessAst: string keys are not supported yet (Stage 4 fixed-key scope)");
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
        throw NQumir::TError("GenJoinProcessAst: key must be a struct");
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

    auto emitCall = call("jt_emit_and_insert", {
        ident("own"), ident("opp"), std::move(keyValue), std::move(ownRowId),
        numI64(isLeft ? 1 : 0), ident("pairs"),
    });

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

} // namespace NQqb::NKernel
