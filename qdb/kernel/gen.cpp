#include <qdb/kernel/gen.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/operator.h>
#include <qumir/location.h>

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace NQqb {
namespace NKernel {

namespace {

NQumir::NAst::TIntegerType::EKind UnsignedIntegerKind(
    NQumir::NAst::TIntegerType::EKind kind)
{
    using TIntegerType = NQumir::NAst::TIntegerType;
    switch (kind) {
        case TIntegerType::I8:
        case TIntegerType::U8:
            return TIntegerType::U8;
        case TIntegerType::I16:
        case TIntegerType::U16:
            return TIntegerType::U16;
        case TIntegerType::I32:
        case TIntegerType::U32:
            return TIntegerType::U32;
        case TIntegerType::I64:
        case TIntegerType::U64:
            return TIntegerType::U64;
    }
    throw std::invalid_argument("unsupported integer kind");
}

NQumir::NAst::TExprPtr KeyValueExpr(
    const std::string& root,
    const std::vector<std::string>& path)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    TExprPtr result = std::make_shared<TIdentExpr>(loc, root);
    for (const auto& field : path) {
        result = std::make_shared<TFieldAccessExpr>(loc, std::move(result), field);
    }
    return result;
}

NQumir::NAst::TExprPtr FloatBitsCall(
    const std::string& root,
    const std::vector<std::string>& path)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    return std::make_shared<TCallExpr>(loc,
        std::make_shared<TIdentExpr>(loc, "qdb_f64_bits"),
        std::vector<TExprPtr>{KeyValueExpr(root, path)});
}

NQumir::NAst::TExprPtr CanonicalFloatBits(
    const std::string& root,
    const std::vector<std::string>& path)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto number = [&](int64_t value) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = u64Type;
        return result;
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    auto masked = [&](int64_t mask) -> TExprPtr {
        return binary("&", FloatBitsCall(root, path), number(mask));
    };
    auto isZero = binary("==", masked(0x7fffffffffffffffLL), number(0));
    auto isNaN = binary("&&",
        binary("==", masked(0x7ff0000000000000LL),
            number(0x7ff0000000000000LL)),
        binary("!=", masked(0x000fffffffffffffLL), number(0)));
    return std::make_shared<TIfExpr>(loc, std::move(isZero), number(0),
        std::make_shared<TIfExpr>(loc, std::move(isNaN),
            number(0x7ff8000000000000LL), FloatBitsCall(root, path)));
}

NQumir::NAst::TExprPtr HashKeyValue(
    const NQumir::NAst::TTypePtr& originalType,
    const std::string& root,
    std::vector<std::string>& path,
    std::vector<NQumir::NAst::TExprPtr>& body,
    size_t& nextTemporary)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto named = TMaybeType<TNamedType>(originalType);
    if (named && (named.Cast()->Name == "StringView" ||
                  named.Cast()->Name == "OwnedString")) {
        return std::make_shared<TCastExpr>(loc,
            std::make_shared<TCallExpr>(loc,
                std::make_shared<TIdentExpr>(loc, "qdb_string_hash"),
                std::vector<TExprPtr>{KeyValueExpr(root, path)}),
            std::make_shared<TIntegerType>(TIntegerType::U64));
    }
    const auto type = UnwrapNamedType(originalType);
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = u64Type;
        return result;
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    auto assign = [&](const std::string& name, TExprPtr value) {
        body.push_back(std::make_shared<TAssignExpr>(
            loc, name, std::move(value)));
    };

    if (auto integer = TMaybeType<TIntegerType>(type)) {
        const std::string name = "key_hash_" + std::to_string(nextTemporary++);
        body.push_back(std::make_shared<TVarStmt>(loc, name, u64Type));
        auto unsignedType = std::make_shared<TIntegerType>(
            UnsignedIntegerKind(integer.Cast()->Kind));
        auto bits = std::make_shared<TCastExpr>(
            loc, KeyValueExpr(root, path), std::move(unsignedType));
        assign(name, std::make_shared<TCastExpr>(loc, std::move(bits), u64Type));
        assign(name, binary("xor", ident(name),
            binary(">>", ident(name), number(12))));
        assign(name, binary("xor", ident(name),
            binary("<<", ident(name), number(25))));
        assign(name, binary("xor", ident(name),
            binary(">>", ident(name), number(27))));
        assign(name, binary("*", ident(name), number(2685821657736338717LL)));
        return ident(name);
    }

    if (TMaybeType<TFloatType>(type)) {
        const std::string name = "key_hash_" + std::to_string(nextTemporary++);
        body.push_back(std::make_shared<TVarStmt>(loc, name, u64Type));
        assign(name, CanonicalFloatBits(root, path));
        assign(name, binary("xor", ident(name),
            binary(">>", ident(name), number(12))));
        assign(name, binary("xor", ident(name),
            binary("<<", ident(name), number(25))));
        assign(name, binary("xor", ident(name),
            binary(">>", ident(name), number(27))));
        assign(name, binary("*", ident(name), number(2685821657736338717LL)));
        return ident(name);
    }

    if (auto structure = TMaybeType<TStructType>(type)) {
        const std::string name = "key_hash_" + std::to_string(nextTemporary++);
        body.push_back(std::make_shared<TVarStmt>(loc, name, u64Type));
        assign(name, number(0));
        for (const auto& [fieldName, fieldType] : structure.Cast()->Fields) {
            if (fieldName.starts_with("__qdb_padding_")) {
                continue;
            }
            path.push_back(fieldName);
            auto fieldHash = HashKeyValue(
                fieldType, root, path, body, nextTemporary);
            path.pop_back();
            // boost-style ordered combine over already mixed field hashes.
            auto combined = binary("+", std::move(fieldHash), number(-7046029254386353131LL));
            combined = binary("+", std::move(combined),
                binary("<<", ident(name), number(6)));
            combined = binary("+", std::move(combined),
                binary(">>", ident(name), number(2)));
            assign(name, binary("xor", ident(name), std::move(combined)));
        }
        return ident(name);
    }

    throw std::invalid_argument(
        "GenKeyOperationFunDecls: unsupported key leaf type " +
        (originalType ? originalType->ToString() : std::string("<null>")));
}

NQumir::NAst::TExprPtr EqualKeyValue(
    const NQumir::NAst::TTypePtr& leftType,
    const NQumir::NAst::TTypePtr& rightType,
    std::vector<std::string>& path)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto leftNamed = TMaybeType<TNamedType>(leftType);
    auto rightNamed = TMaybeType<TNamedType>(rightType);
    const bool leftString = leftNamed &&
        (leftNamed.Cast()->Name == "StringView" ||
         leftNamed.Cast()->Name == "OwnedString");
    const bool rightString = rightNamed &&
        (rightNamed.Cast()->Name == "StringView" ||
         rightNamed.Cast()->Name == "OwnedString");
    if (leftString || rightString) {
        if (!leftString || !rightString) {
            throw std::invalid_argument(
                "GenKeyOperationFunDecls: mismatched string key leaves");
        }
        return std::make_shared<TCallExpr>(loc,
            std::make_shared<TIdentExpr>(loc, "qdb_string_equal"),
            std::vector<TExprPtr>{
                KeyValueExpr("left", path), KeyValueExpr("right", path)});
    }
    const auto left = UnwrapNamedType(leftType);
    const auto right = UnwrapNamedType(rightType);
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    if (TMaybeType<TIntegerType>(left) && TMaybeType<TIntegerType>(right)) {
        return binary("==", KeyValueExpr("left", path), KeyValueExpr("right", path));
    }
    if (TMaybeType<TFloatType>(left) && TMaybeType<TFloatType>(right)) {
        return binary("==",
            CanonicalFloatBits("left", path), CanonicalFloatBits("right", path));
    }
    if (auto leftStruct = TMaybeType<TStructType>(left)) {
        auto rightStruct = TMaybeType<TStructType>(right);
        if (!rightStruct) {
            throw std::invalid_argument(
                "GenKeyOperationFunDecls: mismatched struct key leaves");
        }
        TExprPtr result;
        for (const auto& [fieldName, fieldType] : leftStruct.Cast()->Fields) {
            if (fieldName.starts_with("__qdb_padding_")) {
                continue;
            }
            auto rightField = std::find_if(
                rightStruct.Cast()->Fields.begin(), rightStruct.Cast()->Fields.end(),
                [&](const auto& field) { return field.first == fieldName; });
            if (rightField == rightStruct.Cast()->Fields.end()) {
                throw std::invalid_argument(
                    "GenKeyOperationFunDecls: mismatched struct key fields");
            }
            path.push_back(fieldName);
            auto fieldEqual = EqualKeyValue(fieldType, rightField->second, path);
            path.pop_back();
            result = result
                ? binary("&&", std::move(result), std::move(fieldEqual))
                : std::move(fieldEqual);
        }
        if (!result) {
            throw std::invalid_argument(
                "GenKeyOperationFunDecls: empty struct keys are not supported");
        }
        return result;
    }
    throw std::invalid_argument(
        "GenKeyOperationFunDecls: unsupported equality leaf type " +
        (leftType ? leftType->ToString() : std::string("<null>")));
}

} // namespace

std::vector<NQumir::NAst::TExprPtr> GenKeyOperationFunDecls(
    const TAggregateKeyDescriptor& key)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto boolType = std::make_shared<TBoolType>();

    auto makeHash = [&](const TTypePtr& type) -> TExprPtr {
        std::vector<TExprPtr> body;
        size_t nextTemporary = 0;
        std::vector<std::string> path;
        auto value = HashKeyValue(type, "key", path, body, nextTemporary);
        body.push_back(std::make_shared<TReturnExpr>(loc,
            std::make_shared<TCastExpr>(loc, std::move(value), i64Type)));
        std::vector<TParam> params = {
            std::make_shared<TVarStmt>(loc, "key", type),
        };
        auto function = std::make_shared<TFunDecl>(loc, "rh_hash", std::move(params),
            std::make_shared<TBlockExpr>(loc, std::move(body)), i64Type);
        function->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{type}, i64Type);
        return function;
    };
    auto makeEqual = [&](const TTypePtr& leftType,
                         const TTypePtr& rightType) -> TExprPtr {
        std::vector<TParam> params = {
            std::make_shared<TVarStmt>(loc, "left", leftType),
            std::make_shared<TVarStmt>(loc, "right", rightType),
        };
        std::vector<std::string> path;
        std::vector<TExprPtr> body = {
            std::make_shared<TReturnExpr>(
                loc, EqualKeyValue(leftType, rightType, path)),
        };
        auto function = std::make_shared<TFunDecl>(loc, "rh_key_equal",
            std::move(params), std::make_shared<TBlockExpr>(loc, std::move(body)),
            boolType);
        function->Type = std::make_shared<TFunctionType>(
            std::vector<TTypePtr>{leftType, rightType}, boolType);
        return function;
    };

    std::vector<TExprPtr> result;
    result.push_back(makeHash(key.LookupType));
    if (key.HasDistinctLookupType()) {
        result.push_back(makeHash(key.StoredType));
        result.push_back(makeEqual(key.StoredType, key.LookupType));
    }
    result.push_back(makeEqual(key.StoredType, key.StoredType));
    return result;
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

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto boolType = std::make_shared<TBoolType>();
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    TTypePtr valueType = i64Type;

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
    if (key.IsScalar()) {
        auto ptrKeyType = std::make_shared<TPointerType>(key.KeyType);
        update.push_back(var("keys", ptrKeyType));
        update.push_back(assign(
            "keys", columnData(key.Fields.front().ColumnIndex, ptrKeyType)));
    } else {
        for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
            const auto& keyField = key.Fields[fieldIndex];
            auto ptrFieldType = std::make_shared<TPointerType>(keyField.Type);
            const std::string name = "key_column_" + std::to_string(fieldIndex);
            update.push_back(var(name, ptrFieldType));
            update.push_back(assign(
                name, columnData(keyField.ColumnIndex, ptrFieldType)));
        }
    }
    if (argField) {
        auto arg = std::find_if(inputType.Fields.begin(), inputType.Fields.end(),
            [&](const auto& item) { return item.first == *argField; });
        if (arg == inputType.Fields.end()) {
            throw std::invalid_argument(
                "GenGenericAggregateDispatchAst: unknown argument column '" + *argField + "'");
        }
        auto argType = TMaybeType<TIntegerType>(UnwrapNamedType(arg->second));
        if (!argType) {
            throw std::invalid_argument(
                "GenGenericAggregateDispatchAst: aggregate argument must be integer");
        }
        valueType = arg->second;
        const auto index = static_cast<int32_t>(std::distance(inputType.Fields.begin(), arg));
        auto ptrValueType = std::make_shared<TPointerType>(valueType);
        update.push_back(var("values", ptrValueType));
        update.push_back(assign("values", columnData(index, ptrValueType)));
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
        ? cast(std::make_shared<TIndexExpr>(loc, ident("values"), ident("i")), i64Type)
        : numI64(0);
    TExprPtr keyValue;
    if (key.IsScalar()) {
        keyValue = std::make_shared<TIndexExpr>(
            loc, ident("keys"), ident("i"));
    } else {
        std::vector<TExprPtr> fields;
        auto namedKey = TMaybeType<TNamedType>(key.KeyType);
        auto keyStruct = namedKey
            ? TMaybeType<TStructType>(namedKey.Cast()->UnderlyingType)
            : TMaybeType<TStructType>(key.KeyType);
        if (!keyStruct) {
            throw std::invalid_argument(
                "GenGenericAggregateDispatchAst: composite key must be a struct");
        }
        fields.reserve(keyStruct.Cast()->Fields.size());
        for (const auto& [fieldName, fieldType] : keyStruct.Cast()->Fields) {
            if (fieldName.starts_with("__qdb_padding_")) {
                fields.push_back(number(0, fieldType));
                continue;
            }
            constexpr std::string_view prefix = "key_";
            if (!fieldName.starts_with(prefix)) {
                throw std::invalid_argument(
                    "GenGenericAggregateDispatchAst: unexpected key field '" +
                    fieldName + "'");
            }
            const size_t fieldIndex = std::stoull(fieldName.substr(prefix.size()));
            if (fieldIndex >= key.Fields.size()) {
                throw std::invalid_argument(
                    "GenGenericAggregateDispatchAst: invalid key field '" +
                    fieldName + "'");
            }
            fields.push_back(std::make_shared<TIndexExpr>(
                loc, ident("key_column_" + std::to_string(fieldIndex)), ident("i")));
        }
        keyValue = std::make_shared<TStructConstructExpr>(
            loc, key.KeyType, std::move(fields));
    }
    auto updateCall = call("aht_update", {
        ident("ht"),
        std::move(keyValue),
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

NQumir::NAst::TExprPtr GenGenericAggregateFinalizeAst(
    const TAggregateKeyDescriptor& key,
    NQumir::NAst::TTypePtr hashTableType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrKeyType = std::make_shared<TPointerType>(key.KeyType);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrPtrU8Type = std::make_shared<TPointerType>(ptrU8Type);
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);
    auto hashTableRefType = std::make_shared<TReferenceType>(
        std::make_shared<TNamedType>("HashTable", std::move(hashTableType)));
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "ht", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "output_key_buffers", ptrPtrU8Type),
        std::make_shared<TVarStmt>(loc, "output_buffers", ptrPtrI64Type),
        std::make_shared<TVarStmt>(loc, "output_capacity", i64Type),
    };
    auto stateCall = std::make_shared<TCallExpr>(loc, ident("aht_finalize_states"),
        std::vector<TExprPtr>{
            ident("ht"), ident("output_buffers"), ident("output_capacity"),
        });

    std::vector<TExprPtr> bodyStmts;
    bodyStmts.push_back(std::make_shared<TVarStmt>(loc, "result", i64Type));
    bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, "result", stateCall));

    std::vector<TExprPtr> project;
    project.push_back(std::make_shared<TVarStmt>(loc, "group_keys", ptrKeyType));
    project.push_back(std::make_shared<TAssignExpr>(loc, "group_keys",
        std::make_shared<TCastExpr>(loc,
            std::make_shared<TFieldAccessExpr>(loc, ident("ht"), "GroupKeys"),
            ptrKeyType)));
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        const std::string name = "output_key_" + std::to_string(fieldIndex);
        auto ptrFieldType = std::make_shared<TPointerType>(key.Fields[fieldIndex].Type);
        project.push_back(std::make_shared<TVarStmt>(loc, name, ptrFieldType));
        auto raw = std::make_shared<TIndexExpr>(
            loc, ident("output_key_buffers"),
            std::make_shared<TNumberExpr>(loc, static_cast<int64_t>(fieldIndex)));
        project.push_back(std::make_shared<TAssignExpr>(loc, name,
            std::make_shared<TCastExpr>(loc,
                std::make_shared<TCastExpr>(loc, std::move(raw), i64Type),
                ptrFieldType)));
    }
    project.push_back(std::make_shared<TVarStmt>(loc, "slot", i64Type));
    project.push_back(std::make_shared<TAssignExpr>(
        loc, "slot", std::make_shared<TNumberExpr>(loc, int64_t{0})));

    std::vector<TExprPtr> loopStmts;
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        auto keyValue = std::make_shared<TIndexExpr>(
            loc, ident("group_keys"), ident("slot"));
        TExprPtr value;
        if (key.IsScalar()) {
            value = keyValue;
        } else {
            value = std::make_shared<TFieldAccessExpr>(
                loc, keyValue, "key_" + std::to_string(fieldIndex));
        }
        loopStmts.push_back(std::make_shared<TArrayAssignExpr>(
            loc, "output_key_" + std::to_string(fieldIndex),
            std::vector<TExprPtr>{ident("slot")}, std::move(value)));
    }
    loopStmts.push_back(std::make_shared<TAssignExpr>(loc, "slot",
        std::make_shared<TBinaryExpr>(loc, TOperator("+"), ident("slot"),
            std::make_shared<TNumberExpr>(loc, int64_t{1}))));
    project.push_back(std::make_shared<TWhileStmtExpr>(loc,
        std::make_shared<TBinaryExpr>(loc, TOperator("<"),
            ident("slot"), ident("result")),
        std::make_shared<TBlockExpr>(loc, std::move(loopStmts))));

    bodyStmts.push_back(std::make_shared<TIfExpr>(loc,
        std::make_shared<TBinaryExpr>(loc, TOperator(">="), ident("result"),
            std::make_shared<TNumberExpr>(loc, int64_t{0})),
        std::make_shared<TBlockExpr>(loc, std::move(project)), nullptr));
    bodyStmts.push_back(std::make_shared<TReturnExpr>(loc, ident("result")));
    auto body = std::make_shared<TBlockExpr>(loc, std::move(bodyStmts));
    auto function = std::make_shared<TFunDecl>(
        loc, "agg_finalize", std::move(params), std::move(body), i64Type);
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
