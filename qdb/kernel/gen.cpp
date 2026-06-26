#include <qdb/kernel/gen.h>
#include <qdb/plan/types/nullable.h>

#include <qdb/kernel/column_value.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/operator.h>
#include <qumir/location.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string_view>

namespace NQdb {
namespace NKernel {

namespace {

// Rewrites string-column TIdentExpr nodes to their pre-materialized StringView
// variable names (e.g. "col" → "col_value"). String literals and operator
// comparisons are left as-is — the qumirdb module registers == / != overloads
// for (Named("StringView"), Named("StringView")) and
// (Named("StringView"), TStringType) that the Qumir type-checker resolves.
void SpecializeFilterPredicate(
    NQumir::NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& stringFields,
    const std::unordered_map<std::string, std::string>& stringValues)
{
    using namespace NQumir::NAst;
    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        if (stringFields.contains(ident.Cast()->Name)) {
            expr = std::make_shared<TIdentExpr>(
                expr->Location, stringValues.at(ident.Cast()->Name));
        }
        return;
    }
    for (auto* child : expr->MutableChildren()) {
        SpecializeFilterPredicate(*child, stringFields, stringValues);
    }
}

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
    auto bitcast = [&](TExprPtr e, TTypePtr type) -> TExprPtr {
        return std::make_shared<TBitcastExpr>(loc,
            std::move(e),
            std::move(type));
    };
    auto masked = [&](int64_t mask) -> TExprPtr {
        return binary("&", bitcast(KeyValueExpr(root, path), u64Type), number(mask));
    };
    auto isZero = binary("==", masked(0x7fffffffffffffffLL), number(0));
    auto isNaN = binary("&&",
        binary("==", masked(0x7ff0000000000000LL),
            number(0x7ff0000000000000LL)),
        binary("!=", masked(0x000fffffffffffffLL), number(0)));
    return std::make_shared<TIfExpr>(loc, std::move(isZero), number(0),
        std::make_shared<TIfExpr>(loc, std::move(isNaN),
            number(0x7ff8000000000000LL), bitcast(KeyValueExpr(root, path), u64Type)));
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
        assign(name, binary("^", ident(name),
            binary(">>", ident(name), number(12))));
        assign(name, binary("^", ident(name),
            binary("<<", ident(name), number(25))));
        assign(name, binary("^", ident(name),
            binary(">>", ident(name), number(27))));
        assign(name, binary("*", ident(name), number(2685821657736338717LL)));
        return ident(name);
    }

    if (TMaybeType<TBoolType>(type)) {
        const std::string name = "key_hash_" + std::to_string(nextTemporary++);
        body.push_back(std::make_shared<TVarStmt>(loc, name, u64Type));
        assign(name, std::make_shared<TCastExpr>(loc,
            KeyValueExpr(root, path), u64Type));
        assign(name, binary("*", ident(name), number(2685821657736338717LL)));
        return ident(name);
    }

    if (TMaybeType<TFloatType>(type)) {
        const std::string name = "key_hash_" + std::to_string(nextTemporary++);
        body.push_back(std::make_shared<TVarStmt>(loc, name, u64Type));
        assign(name, CanonicalFloatBits(root, path));
        assign(name, binary("^", ident(name),
            binary(">>", ident(name), number(12))));
        assign(name, binary("^", ident(name),
            binary("<<", ident(name), number(25))));
        assign(name, binary("^", ident(name),
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
            const std::string validityName = fieldName.starts_with("key_")
                ? "valid_" + fieldName.substr(std::string("key_").size())
                : std::string{};
            const bool hasValidity = !validityName.empty() && std::ranges::any_of(
                structure.Cast()->Fields,
                [&](const auto& field) { return field.first == validityName; });
            if (hasValidity) {
                auto validPath = path;
                validPath.push_back(validityName);
                fieldHash = std::make_shared<TIfExpr>(loc,
                    KeyValueExpr(root, validPath), std::move(fieldHash), number(0));
            }
            // boost-style ordered combine over already mixed field hashes.
            auto combined = binary("+", std::move(fieldHash), number(-7046029254386353131LL));
            combined = binary("+", std::move(combined),
                binary("<<", ident(name), number(6)));
            combined = binary("+", std::move(combined),
                binary(">>", ident(name), number(2)));
            assign(name, binary("^", ident(name), std::move(combined)));
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
    if (TMaybeType<TBoolType>(left) && TMaybeType<TBoolType>(right)) {
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
            const std::string validityName = fieldName.starts_with("key_")
                ? "valid_" + fieldName.substr(std::string("key_").size())
                : std::string{};
            const bool hasValidity = !validityName.empty() && std::ranges::any_of(
                leftStruct.Cast()->Fields,
                [&](const auto& field) { return field.first == validityName; });
            if (hasValidity) {
                auto validPath = path;
                validPath.push_back(validityName);
                fieldEqual = binary("||",
                    std::make_shared<TUnaryExpr>(loc, TOperator("!"),
                        KeyValueExpr("left", validPath)),
                    std::move(fieldEqual));
            }
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

bool IsStringHandleType(const NQumir::NAst::TTypePtr& type) {
    auto named = NQumir::NAst::TMaybeType<NQumir::NAst::TNamedType>(type);
    return named && (named.Cast()->Name == "StringView" ||
                     named.Cast()->Name == "OwnedString");
}

NQumir::NAst::TTypePtr FindStringViewType(
    const NQumir::NAst::TTypePtr& originalType)
{
    using namespace NQumir::NAst;
    if (auto named = TMaybeType<TNamedType>(originalType)) {
        if (named.Cast()->Name == "StringView") {
            return named.Cast()->UnderlyingType;
        }
    }
    auto type = UnwrapNamedType(originalType);
    if (auto structure = TMaybeType<TStructType>(type)) {
        for (const auto& [_, fieldType] : structure.Cast()->Fields) {
            if (auto result = FindStringViewType(fieldType)) {
                return result;
            }
        }
    }
    return nullptr;
}

bool ContainsLogicalString(const NQumir::NAst::TTypePtr& originalType) {
    using namespace NQumir::NAst;
    auto type = UnwrapNamedType(originalType);
    if (TMaybeType<TStringType>(type)) {
        return true;
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        return std::any_of(structure.Cast()->Fields.begin(),
            structure.Cast()->Fields.end(), [](const auto& field) {
                return ContainsLogicalString(field.second);
            });
    }
    return false;
}

NQumir::NAst::TExprPtr ZeroValueExpr(
    const NQumir::NAst::TTypePtr& originalType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto zero = std::make_shared<TNumberExpr>(loc, int64_t{0});
    zero->Type = i64Type;
    auto type = UnwrapNamedType(originalType);
    if (TMaybeType<TIntegerType>(type)) {
        auto value = std::make_shared<TNumberExpr>(loc, int64_t{0});
        value->Type = originalType;
        return value;
    }
    if (TMaybeType<TFloatType>(type) || TMaybeType<TBoolType>(type) ||
        TMaybeType<TPointerType>(type)) {
        return std::make_shared<TCastExpr>(loc, std::move(zero), originalType);
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        std::vector<TExprPtr> fields;
        fields.reserve(structure.Cast()->Fields.size());
        for (const auto& [_, fieldType] : structure.Cast()->Fields) {
            fields.push_back(ZeroValueExpr(fieldType));
        }
        return std::make_shared<TStructConstructExpr>(
            loc, originalType, std::move(fields));
    }
    throw std::invalid_argument(
        "ZeroValueExpr: unsupported type " +
        (originalType ? originalType->ToString() : std::string("<null>")));
}

NQumir::NAst::TExprPtr KeyOwnedBytesExpr(
    const NQumir::NAst::TTypePtr& originalType,
    const std::string& root,
    std::vector<std::string>& path)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto zero = [&]() -> TExprPtr {
        auto value = std::make_shared<TNumberExpr>(loc, int64_t{0});
        value->Type = i64Type;
        return value;
    };
    if (IsStringHandleType(originalType)) {
        return std::make_shared<TFieldAccessExpr>(
            loc, KeyValueExpr(root, path), "Size");
    }
    auto type = UnwrapNamedType(originalType);
    if (TMaybeType<TIntegerType>(type) || TMaybeType<TFloatType>(type) ||
        TMaybeType<TBoolType>(type)) {
        return zero();
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        TExprPtr result = zero();
        for (const auto& [fieldName, fieldType] : structure.Cast()->Fields) {
            if (fieldName.starts_with("__qdb_padding_")) {
                continue;
            }
            path.push_back(fieldName);
            auto fieldBytes = KeyOwnedBytesExpr(fieldType, root, path);
            path.pop_back();
            const std::string validityName = fieldName.starts_with("key_")
                ? "valid_" + fieldName.substr(std::string("key_").size())
                : std::string{};
            const bool hasValidity = !validityName.empty() && std::ranges::any_of(
                structure.Cast()->Fields,
                [&](const auto& field) { return field.first == validityName; });
            if (hasValidity) {
                auto validPath = path;
                validPath.push_back(validityName);
                fieldBytes = std::make_shared<TIfExpr>(loc,
                    KeyValueExpr(root, validPath), std::move(fieldBytes), zero());
            }
            result = std::make_shared<TBinaryExpr>(loc, TOperator("+"),
                std::move(result), std::move(fieldBytes));
        }
        return result;
    }
    throw std::invalid_argument(
        "GenKeyOwnershipFunDecls: unsupported key leaf type " +
        (originalType ? originalType->ToString() : std::string("<null>")));
}

NQumir::NAst::TExprPtr CloneKeyValue(
    const NQumir::NAst::TTypePtr& lookupType,
    const NQumir::NAst::TTypePtr& storedType,
    const std::string& root,
    std::vector<std::string>& path,
    std::vector<NQumir::NAst::TExprPtr>& body,
    size_t& nextTemporary)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto field = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TFieldAccessExpr>(
            loc, KeyValueExpr(root, path), name);
    };

    if (IsStringHandleType(lookupType) && IsStringHandleType(storedType)) {
        const std::string dataName =
            "owned_data_" + std::to_string(nextTemporary++);
        const std::string valueName =
            "owned_value_" + std::to_string(nextTemporary++);
        const std::string copyName =
            "owned_copy_" + std::to_string(nextTemporary++);
        body.push_back(std::make_shared<TVarStmt>(loc, dataName, ptrU8Type));
        auto address = std::make_shared<TBinaryExpr>(loc, TOperator("+"),
            std::make_shared<TCastExpr>(loc, ident("owned_buffer"), i64Type),
            ident("owned_offset"));
        body.push_back(std::make_shared<TAssignExpr>(loc, dataName,
            std::make_shared<TCastExpr>(loc, std::move(address), ptrU8Type)));
        body.push_back(std::make_shared<TVarStmt>(loc, copyName, i64Type));
        body.push_back(std::make_shared<TAssignExpr>(loc, copyName,
            std::make_shared<TCallExpr>(loc,
                ident("qdb_string_copy_bytes"),
                std::vector<TExprPtr>{
                    ident(dataName), field("Data"), field("Size")})));
        body.push_back(std::make_shared<TVarStmt>(loc, valueName, storedType));
        body.push_back(std::make_shared<TAssignExpr>(loc, valueName,
            std::make_shared<TStructConstructExpr>(loc, storedType,
                std::vector<TExprPtr>{ident(dataName), field("Size")})));
        body.push_back(std::make_shared<TAssignExpr>(loc, "owned_offset",
            std::make_shared<TBinaryExpr>(loc, TOperator("+"),
                ident("owned_offset"), field("Size"))));
        return ident(valueName);
    }

    auto lookup = UnwrapNamedType(lookupType);
    auto stored = UnwrapNamedType(storedType);
    if ((TMaybeType<TIntegerType>(lookup) && TMaybeType<TIntegerType>(stored)) ||
        (TMaybeType<TFloatType>(lookup) && TMaybeType<TFloatType>(stored)) ||
        (TMaybeType<TBoolType>(lookup) && TMaybeType<TBoolType>(stored))) {
        return KeyValueExpr(root, path);
    }
    if (auto storedStruct = TMaybeType<TStructType>(stored)) {
        auto lookupStruct = TMaybeType<TStructType>(lookup);
        if (!lookupStruct) {
            throw std::invalid_argument(
                "GenKeyOwnershipFunDecls: mismatched struct key types");
        }
        std::vector<TExprPtr> fields;
        fields.reserve(storedStruct.Cast()->Fields.size());
        for (const auto& [fieldName, fieldType] : storedStruct.Cast()->Fields) {
            if (fieldName.starts_with("__qdb_padding_")) {
                auto padding = std::make_shared<TNumberExpr>(loc, int64_t{0});
                padding->Type = fieldType;
                fields.push_back(std::move(padding));
                continue;
            }
            auto lookupField = std::find_if(
                lookupStruct.Cast()->Fields.begin(), lookupStruct.Cast()->Fields.end(),
                [&](const auto& item) { return item.first == fieldName; });
            if (lookupField == lookupStruct.Cast()->Fields.end()) {
                throw std::invalid_argument(
                    "GenKeyOwnershipFunDecls: mismatched struct key fields");
            }
            path.push_back(fieldName);
            fields.push_back(CloneKeyValue(lookupField->second, fieldType,
                root, path, body, nextTemporary));
            path.pop_back();
        }
        return std::make_shared<TStructConstructExpr>(
            loc, storedType, std::move(fields));
    }
    throw std::invalid_argument(
        "GenKeyOwnershipFunDecls: unsupported clone type " +
        (lookupType ? lookupType->ToString() : std::string("<null>")));
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

std::vector<NQumir::NAst::TExprPtr> GenKeyOwnershipFunDecls(
    const TAggregateKeyDescriptor& key)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);

    std::vector<std::string> path;
    auto bytesExpr = KeyOwnedBytesExpr(key.LookupType, "key", path);
    std::vector<TParam> bytesParams = {
        std::make_shared<TVarStmt>(loc, "key", key.LookupType),
    };
    auto bytes = std::make_shared<TFunDecl>(loc, "key_owned_bytes",
        std::move(bytesParams), std::make_shared<TBlockExpr>(loc,
            std::vector<TExprPtr>{
                std::make_shared<TReturnExpr>(loc, std::move(bytesExpr))}),
        i64Type);
    bytes->Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{key.LookupType}, i64Type);

    std::vector<TExprPtr> cloneBody;
    TExprPtr cloneValue;
    if (key.HasDistinctLookupType()) {
        auto offset = std::make_shared<TVarStmt>(loc, "owned_offset", i64Type);
        offset->Init = std::make_shared<TNumberExpr>(loc, int64_t{0});
        cloneBody.push_back(std::move(offset));
        size_t nextTemporary = 0;
        path.clear();
        cloneValue = CloneKeyValue(key.LookupType, key.StoredType,
            "key", path, cloneBody, nextTemporary);
    } else {
        cloneValue = std::make_shared<TIdentExpr>(loc, "key");
    }
    cloneBody.push_back(
        std::make_shared<TReturnExpr>(loc, std::move(cloneValue)));
    std::vector<TParam> cloneParams = {
        std::make_shared<TVarStmt>(loc, "key", key.LookupType),
        std::make_shared<TVarStmt>(loc, "owned_buffer", ptrU8Type),
    };
    auto clone = std::make_shared<TFunDecl>(loc, "key_clone_owned",
        std::move(cloneParams),
        std::make_shared<TBlockExpr>(loc, std::move(cloneBody)), key.StoredType);
    clone->Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{key.LookupType, ptrU8Type}, key.StoredType);

    return {std::move(bytes), std::move(clone)};
}

void SubstFieldsInPlace(
    NQumir::NAst::TExprPtr& expr,
    const std::unordered_map<std::string, std::string>& fieldValues)
{
    if (!expr) {
        return;
    }
    if (auto node = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(expr)) {
        if (auto it = fieldValues.find(node.Cast()->Name);
            it != fieldValues.end()) {
            expr = std::make_shared<NQumir::NAst::TIdentExpr>(
                expr->Location, it->second);
            return;
        }
        return;
    }
    for (auto* child : expr->MutableChildren()) {
        SubstFieldsInPlace(*child, fieldValues);
    }
}

struct TFilterTruthAst {
    std::string State;
};

NQumir::NAst::TExprPtr FilterExprValidity(
    const NQumir::NAst::TExprPtr& expr,
    const std::unordered_map<std::string, std::string>& validityNames)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    TExprPtr result = std::make_shared<TNumberExpr>(loc, true);
    result->Type = std::make_shared<TBoolType>();
    std::unordered_set<std::string> used;
    auto visit = [&](auto&& self, const TExprPtr& node) -> void {
        if (auto ident = TMaybeNode<TIdentExpr>(node)) {
            auto it = validityNames.find(ident.Cast()->Name);
            if (it != validityNames.end() && used.insert(it->second).second) {
                result = std::make_shared<TCallExpr>(loc,
                    std::make_shared<TIdentExpr>(loc, "qdb_sql_bool_and"),
                    std::vector<TExprPtr>{
                        std::make_shared<TCastExpr>(loc, std::move(result),
                            std::make_shared<TIntegerType>()),
                        std::make_shared<TCastExpr>(loc,
                            std::make_shared<TIdentExpr>(loc, it->second),
                            std::make_shared<TIntegerType>())});
                result = std::make_shared<TBinaryExpr>(loc, TOperator("=="),
                    std::move(result),
                    std::make_shared<TNumberExpr>(loc, int64_t{1}));
            }
            return;
        }
        for (const auto& child : node->Children()) {
            self(self, child);
        }
    };
    visit(visit, expr);
    return result;
}

TFilterTruthAst BuildFilterTruthAst(
    NQumir::NAst::TExprPtr expr,
    const std::unordered_map<std::string, std::string>& validityNames,
    std::vector<NQumir::NAst::TExprPtr>& body,
    size_t& nextTemporary)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = std::make_shared<TIntegerType>();
        return result;
    };
    auto materialize = [&](TExprPtr state) {
        const std::string suffix = std::to_string(nextTemporary++);
        TFilterTruthAst result{
            .State = "filter_truth_state_" + suffix,
        };
        auto i64Type = std::make_shared<TIntegerType>();
        body.push_back(std::make_shared<TVarStmt>(loc, result.State, i64Type));
        body.push_back(std::make_shared<TAssignExpr>(
            loc, result.State, std::move(state)));
        return result;
    };
    auto call = [&](const char* name, std::vector<TExprPtr> args) -> TExprPtr {
        return std::make_shared<TCallExpr>(loc,
            std::make_shared<TIdentExpr>(loc, name), std::move(args));
    };
    auto bitcast = [&](TExprPtr value, TTypePtr type) -> TExprPtr {
        return std::make_shared<TBitcastExpr>(loc, std::move(value), std::move(type));
    };
    auto binaryExpr = TMaybeNode<TBinaryExpr>(expr);
    if (binaryExpr && (binaryExpr.Cast()->Operator == "&&" ||
                      binaryExpr.Cast()->Operator == "||")) {
        const bool isAnd = binaryExpr.Cast()->Operator == "&&";
        auto left = BuildFilterTruthAst(
            binaryExpr.Cast()->Left, validityNames, body, nextTemporary);
        auto right = BuildFilterTruthAst(
            binaryExpr.Cast()->Right, validityNames, body, nextTemporary);
        return materialize(call(isAnd ? "qdb_sql_bool_and" : "qdb_sql_bool_or",
            {ident(left.State), ident(right.State)}));
    }
    if (auto unary = TMaybeNode<TUnaryExpr>(expr);
        unary && unary.Cast()->Operator == "!") {
        auto operand = BuildFilterTruthAst(
            unary.Cast()->Operand, validityNames, body, nextTemporary);
        return materialize(call("qdb_sql_bool_not", {ident(operand.State)}));
    }
    auto value = std::make_shared<TIfExpr>(loc, std::move(expr), number(1), number(0));
    auto state = std::make_shared<TIfExpr>(loc,
        FilterExprValidity(value, validityNames), std::move(value), number(2));
    return materialize(std::move(state));
}

bool UsesNullableValue(
    const NQumir::NAst::TExprPtr& expr,
    const std::unordered_map<std::string, std::string>& validityNames)
{
    using namespace NQumir::NAst;
    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        return validityNames.contains(ident.Cast()->Name);
    }
    return std::ranges::any_of(expr->Children(), [&](const auto& child) {
        return UsesNullableValue(child, validityNames);
    });
}

// Kernel takes (ref TRowSet) directly. Column data pointers are extracted via
// TRowSet.Columns[colIdx].Data with a two-step cast: <ptr i8> -> i64 -> <ptr T>.
NQumir::NAst::TExprPtr GenFilterKernelAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType,
    std::vector<std::shared_ptr<std::string>>& /*literalStorage*/)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    std::unordered_map<std::string, std::string> fixedValues;
    std::unordered_set<std::string> stringFields;
    std::unordered_map<std::string, std::string> stringValues;
    for (const auto& [name, type] : inputType.Fields) {
        if (TMaybeType<TStringType>(
                UnwrapNamedType(UnwrapNullableType(type)))) {
            stringFields.insert(name);
            stringValues.emplace(name, name + "_value");
        } else {
            fixedValues.emplace(name, name + "_value");
        }
    }

    SpecializeFilterPredicate(
        predicate, stringFields, stringValues);
    SubstFieldsInPlace(predicate, fixedValues);

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
    auto columnValueType = [&]() -> TTypePtr {
        if (auto pointer = TMaybeType<TPointerType>(ptrColumnType)) {
            return pointer.Cast()->PointeeType;
        }
        return columnType;
    }();
    bodyStmts.push_back(std::make_shared<TVarStmt>(loc, "cols", ptrColumnType));
    bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, "cols", fieldOf("Columns")));

    // Bind every input field through the common nullable column materializer.
    std::vector<TExprPtr> loopSetup;
    std::unordered_map<std::string, std::string> validityNames;
    for (const auto& [name, type] : inputType.Fields) {
        int32_t idx = fieldIndices.at(name);
        auto colElem = std::make_shared<TIndexExpr>(loc,
            std::make_shared<TIdentExpr>(loc, "cols"),
            std::make_shared<TNumberExpr>(loc, int64_t(idx)));
        bodyStmts.push_back(std::make_shared<TVarStmt>(loc, name, columnValueType));
        bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, name, colElem));
        const std::string prefix = name + "_filter";
        auto materialized = BuildColumnValueAst(
            name, "i", prefix, type, stringViewType);
        loopSetup.insert(loopSetup.end(),
            std::make_move_iterator(materialized.Setup.begin()),
            std::make_move_iterator(materialized.Setup.end()));
        const std::string valueName = TMaybeType<TStringType>(
            UnwrapNamedType(UnwrapNullableType(type)))
            ? stringValues.at(name)
            : fixedValues.at(name);
        if (IsNullableType(type)) {
            validityNames.emplace(valueName, prefix + "_valid");
        }
        loopSetup.push_back(std::make_shared<TVarStmt>(
            loc, valueName, materialized.ValueType));
        loopSetup.push_back(std::make_shared<TAssignExpr>(
            loc, valueName, std::move(materialized.Value)));
    }

    // Loop
    auto varI = std::make_shared<TVarStmt>(loc, "i", std::make_shared<TIntegerType>());
    auto initI = std::make_shared<TAssignExpr>(loc, "i",
        std::make_shared<TNumberExpr>(loc, int64_t(0)));
    auto cond = std::make_shared<TBinaryExpr>(loc, TOperator("<"),
        std::make_shared<TIdentExpr>(loc, "i"),
        std::make_shared<TIdentExpr>(loc, "n"));
    TExprPtr selected;
    if (validityNames.empty() || !UsesNullableValue(predicate, validityNames)) {
        selected = std::move(predicate);
    } else {
        size_t nextTruthTemporary = 0;
        auto truth = BuildFilterTruthAst(
            std::move(predicate), validityNames, loopSetup, nextTruthTemporary);
        selected = std::make_shared<TBinaryExpr>(loc, TOperator("=="),
            std::make_shared<TIdentExpr>(loc, truth.State),
            std::make_shared<TNumberExpr>(loc, int64_t{1}));
    }
    auto castedPred = std::make_shared<TCastExpr>(loc, std::move(selected),
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
    loopSetup.push_back(std::move(writeSel));
    loopSetup.push_back(std::move(incrI));
    bodyStmts.push_back(std::make_shared<TWhileStmtExpr>(loc, cond,
        std::make_shared<TBlockExpr>(loc, std::move(loopSetup))));

    auto funBody = std::make_shared<TBlockExpr>(loc, std::move(bodyStmts));
    auto funDecl = std::make_shared<TFunDecl>(loc, "<kernel>",
        std::move(params), funBody, std::make_shared<TVoidType>());

    return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{funDecl});
}

namespace {

// Recursively collects all identifier names referenced in an expression.
void CollectIdentNames(
    const NQumir::NAst::TExprPtr& expr,
    std::unordered_set<std::string>& out)
{
    using namespace NQumir::NAst;
    if (!expr) {
        return;
    }
    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        out.insert(ident.Cast()->Name);
        return;
    }
    for (const auto& child : expr->Children()) {
        CollectIdentNames(child, out);
    }
}

// Deep-copies a filter predicate AST so the in-place rewrites
// (SpecializeFilterPredicate / SubstFieldsInPlace) don't mutate a predicate
// shared across multiple compilations (CompileJoin compiles each entry from a
// fresh program). Covers the node kinds that appear in filter predicates.
NQumir::NAst::TExprPtr CloneFilterExpr(const NQumir::NAst::TExprPtr& expr) {
    using namespace NQumir::NAst;
    if (!expr) {
        return nullptr;
    }
    const NQumir::TLocation loc = expr->Location;
    TExprPtr out;
    if (auto n = TMaybeNode<TIdentExpr>(expr)) {
        out = std::make_shared<TIdentExpr>(loc, n.Cast()->Name);
    } else if (auto n = TMaybeNode<TNumberExpr>(expr)) {
        out = n.Cast()->IsFloat()
            ? std::make_shared<TNumberExpr>(loc, n.Cast()->FloatValue)
            : std::make_shared<TNumberExpr>(loc, n.Cast()->IntValue);
    } else if (auto n = TMaybeNode<TStringLiteralExpr>(expr)) {
        out = std::make_shared<TStringLiteralExpr>(loc, n.Cast()->Value);
    } else if (auto n = TMaybeNode<TUnaryExpr>(expr)) {
        out = std::make_shared<TUnaryExpr>(loc, n.Cast()->Operator,
            CloneFilterExpr(n.Cast()->Operand));
    } else if (auto n = TMaybeNode<TBinaryExpr>(expr)) {
        out = std::make_shared<TBinaryExpr>(loc, n.Cast()->Operator,
            CloneFilterExpr(n.Cast()->Left), CloneFilterExpr(n.Cast()->Right));
    } else if (auto n = TMaybeNode<TCallExpr>(expr)) {
        std::vector<TExprPtr> args;
        args.reserve(n.Cast()->Args.size());
        for (const auto& a : n.Cast()->Args) {
            args.push_back(CloneFilterExpr(a));
        }
        out = std::make_shared<TCallExpr>(loc,
            CloneFilterExpr(n.Cast()->Callee), std::move(args));
    } else if (auto n = TMaybeNode<TCastExpr>(expr)) {
        out = std::make_shared<TCastExpr>(loc,
            CloneFilterExpr(n.Cast()->Operand), n.Cast()->Type);
    } else if (auto n = TMaybeNode<TIndexExpr>(expr)) {
        out = std::make_shared<TIndexExpr>(loc,
            CloneFilterExpr(n.Cast()->Collection), CloneFilterExpr(n.Cast()->Index));
    } else {
        throw std::runtime_error(
            "residual filter: cannot clone predicate node '" +
            std::string(expr->NodeName()) + "'");
    }
    out->Type = expr->Type;
    return out;
}

} // namespace

// Residual join filter: evaluates `predicate` on a single (left_row, right_row)
// pair. Columns are read directly from the two row stores (contiguous TRowSet
// arrays) by decoding the packed row IDs. Mirrors GenFilterKernelAst's column
// binding, but with no loop and per-side store/row selection: inner-schema
// fields with index < leftFieldCount come from left_store at left_row, the rest
// from right_store at right_row.
NQumir::NAst::TExprPtr GenJoinResidualFilterAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& innerType,
    size_t leftFieldCount,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto boolType = std::make_shared<TBoolType>();

    // Work on a private copy — CompileJoin compiles each entry from a fresh
    // program, so the shared predicate must not be mutated in place.
    predicate = CloneFilterExpr(predicate);

    // Per-field value-variable names (matching GenFilterKernelAst's scheme).
    std::unordered_map<std::string, std::string> fixedValues;
    std::unordered_set<std::string> stringFields;
    std::unordered_map<std::string, std::string> stringValues;
    for (const auto& [name, type] : innerType.Fields) {
        if (TMaybeType<TStringType>(UnwrapNamedType(UnwrapNullableType(type)))) {
            stringFields.insert(name);
            stringValues.emplace(name, name + "_value");
        } else {
            fixedValues.emplace(name, name + "_value");
        }
    }

    // Which columns the predicate actually touches (collect before renaming).
    std::unordered_set<std::string> referenced;
    CollectIdentNames(predicate, referenced);

    SpecializeFilterPredicate(predicate, stringFields, stringValues);
    SubstFieldsInPlace(predicate, fixedValues);

    auto rowSetPtrType = std::make_shared<TPointerType>(
        std::make_shared<TNamedType>("TRowSet", rowSetType));
    auto rowSetRefType = std::make_shared<TReferenceType>(
        std::make_shared<TNamedType>("TRowSet", rowSetType));
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "left_store", rowSetPtrType),
        std::make_shared<TVarStmt>(loc, "right_store", rowSetPtrType),
        std::make_shared<TVarStmt>(loc, "stream_left_batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "stream_right_batch", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "left_row_id", i64Type),
        std::make_shared<TVarStmt>(loc, "right_row_id", i64Type),
    };

    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto numI64 = [&](int64_t value) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = i64Type;
        return result;
    };
    auto binary = [&](const char* op, TExprPtr l, TExprPtr r) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(loc, TOperator(op),
            std::move(l), std::move(r));
    };
    auto var = [&](const std::string& name, TTypePtr type) -> TExprPtr {
        return std::make_shared<TVarStmt>(loc, name, std::move(type));
    };
    auto assign = [&](const std::string& name, TExprPtr value) -> TExprPtr {
        return std::make_shared<TAssignExpr>(loc, name, std::move(value));
    };
    auto block = [&](std::vector<TExprPtr> stmts) -> TExprPtr {
        return std::make_shared<TBlockExpr>(loc, std::move(stmts));
    };

    // Columns field type (pointer) and its pointee (one TColumn value).
    auto ptrColumnType = [&]() -> TTypePtr {
        auto* rs = static_cast<TStructType*>(rowSetType.get());
        for (const auto& [name, type] : rs->Fields) {
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

    std::vector<TExprPtr> body;
    // Decode packed row IDs: batch = id >> 32, row = id & 0xffffffff.
    body.push_back(var("left_batch", i64Type));
    body.push_back(assign("left_batch", binary(">>", ident("left_row_id"), numI64(32))));
    body.push_back(var("left_row", i64Type));
    body.push_back(assign("left_row", binary("&", ident("left_row_id"), numI64(0xffffffff))));
    body.push_back(var("right_batch", i64Type));
    body.push_back(assign("right_batch", binary(">>", ident("right_row_id"), numI64(32))));
    body.push_back(var("right_row", i64Type));
    body.push_back(assign("right_row", binary("&", ident("right_row_id"), numI64(0xffffffff))));

    // Per-side Columns pointers. Batch index -1 means the side is the current
    // stream batch and is not present in the row store.
    auto storeColumns = [&](const char* store, const char* batchVar) -> TExprPtr {
        auto rs = std::make_shared<TIndexExpr>(loc, ident(store), ident(batchVar));
        return std::make_shared<TFieldAccessExpr>(loc, rs, "Columns");
    };
    auto streamColumns = [&](const char* streamBatch) -> TExprPtr {
        return std::make_shared<TFieldAccessExpr>(loc, ident(streamBatch), "Columns");
    };
    body.push_back(var("left_cols", ptrColumnType));
    body.push_back(std::make_shared<TIfExpr>(loc,
        binary("==", ident("left_batch"), numI64(-1)),
        block({assign("left_cols", streamColumns("stream_left_batch"))}),
        block({assign("left_cols", storeColumns("left_store", "left_batch"))})));
    body.push_back(var("right_cols", ptrColumnType));
    body.push_back(std::make_shared<TIfExpr>(loc,
        binary("==", ident("right_batch"), numI64(-1)),
        block({assign("right_cols", streamColumns("stream_right_batch"))}),
        block({assign("right_cols", storeColumns("right_store", "right_batch"))})));

    // Bind only the referenced columns through the shared materializer.
    std::unordered_map<std::string, std::string> validityNames;
    int32_t fieldIndex = 0;
    for (const auto& [name, type] : innerType.Fields) {
        const int32_t idx = fieldIndex++;
        if (!referenced.contains(name)) {
            continue;
        }
        const bool isLeft = static_cast<size_t>(idx) < leftFieldCount;
        const char* colsVar = isLeft ? "left_cols" : "right_cols";
        const char* rowVar = isLeft ? "left_row" : "right_row";
        const int32_t colIdx = isLeft ? idx
            : idx - static_cast<int32_t>(leftFieldCount);

        auto colElem = std::make_shared<TIndexExpr>(loc,
            ident(colsVar), numI64(colIdx));
        body.push_back(var(name, columnValueType));
        body.push_back(assign(name, colElem));

        const std::string prefix = name + "_residual";
        auto materialized = BuildColumnValueAst(name, rowVar, prefix, type, stringViewType);
        body.insert(body.end(),
            std::make_move_iterator(materialized.Setup.begin()),
            std::make_move_iterator(materialized.Setup.end()));
        const std::string valueName = TMaybeType<TStringType>(
            UnwrapNamedType(UnwrapNullableType(type)))
            ? stringValues.at(name)
            : fixedValues.at(name);
        if (IsNullableType(type)) {
            validityNames.emplace(valueName, prefix + "_valid");
        }
        body.push_back(var(valueName, materialized.ValueType));
        body.push_back(assign(valueName, std::move(materialized.Value)));
    }

    // Evaluate the predicate (with three-valued-logic truth when nullable).
    TExprPtr result;
    if (validityNames.empty() || !UsesNullableValue(predicate, validityNames)) {
        result = std::move(predicate);
    } else {
        size_t nextTruthTemporary = 0;
        auto truth = BuildFilterTruthAst(
            std::move(predicate), validityNames, body, nextTruthTemporary);
        result = std::make_shared<TBinaryExpr>(loc, TOperator("=="),
            ident(truth.State), numI64(1));
    }
    auto castedResult = std::make_shared<TCastExpr>(loc, std::move(result), boolType);
    body.push_back(std::make_shared<TReturnExpr>(loc, std::move(castedResult)));

    return std::make_shared<TFunDecl>(loc, "jt_residual_filter",
        std::move(params), std::make_shared<TBlockExpr>(loc, std::move(body)),
        boolType);
}

// Project kernel for COMPUTED columns. Mirrors GenFilterKernelAst's column
// binding/materialization (column refs in the exprs are rewritten to {name}_value
// temps), but instead of writing a selection mask it writes each computed
// expression to its output buffer: out[k][i] = cast(<expr_k>, computedTypes[k]).
NQumir::NAst::TExprPtr GenProjectKernelAst(
    std::vector<NQumir::NAst::TExprPtr> computedExprs,
    const std::vector<NQumir::NAst::TTypePtr>& computedTypes,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType,
    std::vector<std::shared_ptr<std::string>>& literalStorage)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto numI64 = [&](int64_t v) -> TExprPtr {
        auto r = std::make_shared<TNumberExpr>(loc, v);
        r->Type = std::make_shared<TIntegerType>();
        return r;
    };
    auto var = [&](const std::string& name, TTypePtr type) -> TExprPtr {
        return std::make_shared<TVarStmt>(loc, name, std::move(type));
    };
    auto assign = [&](const std::string& name, TExprPtr value) -> TExprPtr {
        return std::make_shared<TAssignExpr>(loc, name, std::move(value));
    };
    auto cast = [&](TExprPtr e, TTypePtr t) -> TExprPtr {
        return std::make_shared<TCastExpr>(loc, std::move(e), std::move(t));
    };

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrPtrU8Type = std::make_shared<TPointerType>(ptrU8Type);

    // Collect column names referenced in computed expressions before substitution.
    std::unordered_set<std::string> referencedCols;
    {
        std::function<void(const TExprPtr&)> collect = [&](const TExprPtr& e) {
            if (!e) return;
            if (auto id = TMaybeNode<TIdentExpr>(e)) {
                referencedCols.insert(id.Cast()->Name);
                return;
            }
            for (const auto& child : e->Children()) collect(child);
        };
        for (const auto& expr : computedExprs) collect(expr);
    }

    // Value-variable naming, identical to the filter kernel.
    std::unordered_map<std::string, std::string> fixedValues;
    std::unordered_set<std::string> stringFields;
    std::unordered_map<std::string, std::string> stringValues;
    for (const auto& [name, type] : inputType.Fields) {
        if (TMaybeType<TStringType>(UnwrapNamedType(UnwrapNullableType(type)))) {
            stringFields.insert(name);
            stringValues.emplace(name, name + "_value");
        } else {
            fixedValues.emplace(name, name + "_value");
        }
    }
    for (auto& expr : computedExprs) {
        SpecializeFilterPredicate(expr, stringFields, stringValues);
        SubstFieldsInPlace(expr, fixedValues);
    }

    auto rowSetRefType = std::make_shared<TReferenceType>(rowSetType);
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "rowSet", rowSetRefType),
        std::make_shared<TVarStmt>(loc, "out", ptrPtrU8Type),
    };

    std::vector<TExprPtr> bodyStmts;
    auto identRowSet = ident("rowSet");
    auto fieldOf = [&](const std::string& name) {
        return std::make_shared<TFieldAccessExpr>(loc, identRowSet, name);
    };
    bodyStmts.push_back(var("n", i64Type));
    bodyStmts.push_back(assign("n", fieldOf("RowCount")));

    auto ptrColumnType = [&]() -> TTypePtr {
        auto* rs = static_cast<TStructType*>(rowSetType.get());
        for (const auto& [name, type] : rs->Fields) {
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
    bodyStmts.push_back(var("cols", ptrColumnType));
    bodyStmts.push_back(assign("cols", fieldOf("Columns")));

    // Bind + materialize only columns referenced in computed expressions.
    std::vector<TExprPtr> loopSetup;
    for (const auto& [name, type] : inputType.Fields) {
        if (!referencedCols.contains(name)) {
            continue;
        }
        const int32_t idx = fieldIndices.at(name);
        auto colElem = std::make_shared<TIndexExpr>(loc, ident("cols"), numI64(idx));
        bodyStmts.push_back(var(name, columnValueType));
        bodyStmts.push_back(assign(name, colElem));
        auto materialized = BuildColumnValueAst(name, "i", name + "_proj", type, stringViewType);
        loopSetup.insert(loopSetup.end(),
            std::make_move_iterator(materialized.Setup.begin()),
            std::make_move_iterator(materialized.Setup.end()));
        const std::string valueName = stringFields.contains(name)
            ? stringValues.at(name) : fixedValues.at(name);
        loopSetup.push_back(var(valueName, materialized.ValueType));
        loopSetup.push_back(assign(valueName, std::move(materialized.Value)));
    }

    // Hoist typed output pointers: out_k = (<ptr T_k>) out[k].
    for (size_t k = 0; k < computedExprs.size(); ++k) {
        auto ptrTk = std::make_shared<TPointerType>(computedTypes[k]);
        auto outK = std::make_shared<TIndexExpr>(loc, ident("out"), numI64(int64_t(k)));
        bodyStmts.push_back(var("out_" + std::to_string(k), ptrTk));
        bodyStmts.push_back(assign("out_" + std::to_string(k),
            cast(cast(outK, i64Type), ptrTk)));
    }

    bodyStmts.push_back(var("i", i64Type));
    bodyStmts.push_back(assign("i", numI64(0)));
    for (size_t k = 0; k < computedExprs.size(); ++k) {
        loopSetup.push_back(std::make_shared<TArrayAssignExpr>(loc,
            "out_" + std::to_string(k), std::vector<TExprPtr>{ident("i")},
            cast(std::move(computedExprs[k]), computedTypes[k])));
    }
    loopSetup.push_back(assign("i",
        std::make_shared<TBinaryExpr>(loc, TOperator("+"), ident("i"), numI64(1))));
    auto cond = std::make_shared<TBinaryExpr>(loc, TOperator("<"), ident("i"), ident("n"));
    bodyStmts.push_back(std::make_shared<TWhileStmtExpr>(loc, cond,
        std::make_shared<TBlockExpr>(loc, std::move(loopSetup))));

    auto funBody = std::make_shared<TBlockExpr>(loc, std::move(bodyStmts));
    auto funDecl = std::make_shared<TFunDecl>(loc, "<project>",
        std::move(params), funBody, std::make_shared<TVoidType>());
    return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{funDecl});
}

TAggReducerLayout BuildAggReducerLayout(
    const std::vector<std::string>& funcs,
    const std::vector<TAggArg>& args)
{
    TAggReducerLayout layout;
    layout.Reducers.reserve(funcs.size());
    int nextBufIdx = static_cast<int>(funcs.size());
    for (size_t i = 0; i < funcs.size(); ++i) {
        TAggReducerInfo info;
        info.Func = funcs[i];
        const TAggArg& arg = args[i];
        info.HasArg = arg.ColumnIndex >= 0;
        info.ArgColumnIndex = arg.ColumnIndex;
        info.NeedsValidity = arg.IsNullable && info.HasArg;
        const bool isAggFunc =
            info.Func == "sum" || info.Func == "min" || info.Func == "max";
        info.IsFloat = arg.IsFloat && info.HasArg && isAggFunc;
        info.IsNullableOutput = info.NeedsValidity && isAggFunc;
        if (info.IsNullableOutput) {
            info.ValidBufIdx = nextBufIdx++;
        }
        layout.Reducers.push_back(std::move(info));
    }
    layout.NumAggBuffers = static_cast<size_t>(nextBufIdx);
    return layout;
}

NQumir::NAst::TExprPtr GenGenericAggregateDispatchAst(
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
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
    auto bitcast = [&](TExprPtr expr, TTypePtr type) -> TExprPtr {
        return std::make_shared<TBitcastExpr>(loc, std::move(expr), std::move(type));
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
    auto columnValueType = [&]() -> TTypePtr {
        if (auto pointer = TMaybeType<TPointerType>(ptrColumnType)) {
            return pointer.Cast()->PointeeType;
        }
        return columnType;
    }();
    auto columnAt = [&](int32_t index) -> TExprPtr {
        return std::make_shared<TIndexExpr>(loc, ident("cols"), numI64(index));
    };
    auto columnData = [&](int32_t index, TTypePtr pointerType) -> TExprPtr {
        auto column = columnAt(index);
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
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        const auto& keyField = key.Fields[fieldIndex];
        const std::string name = "key_column_" + std::to_string(fieldIndex);
        update.push_back(var(name, columnValueType));
        update.push_back(assign(name, columnAt(keyField.ColumnIndex)));
    }
    auto stringViewType = FindStringViewType(key.LookupType);

    // Materialize each DISTINCT argument column once. Non-nullable columns
    // expose a typed data pointer (values_<idx>); nullable columns go through
    // the shared TColumn materializer (arg_column_<idx>). Reducers then read
    // their own column by index (layout's ArgColumnIndex).
    struct TArgColumn {
        bool Float = false;
        bool Nullable = false;
        std::optional<TColumnValueAst> Mat; // set for nullable columns
    };
    std::map<int32_t, TArgColumn> argColumns;
    for (const auto& r : layout.Reducers) {
        if (r.ArgColumnIndex < 0 || argColumns.contains(r.ArgColumnIndex)) {
            continue;
        }
        const int32_t idx = r.ArgColumnIndex;
        const auto& colType = inputType.Fields[idx].second;
        TArgColumn ac;
        ac.Float = static_cast<bool>(
            TMaybeType<TFloatType>(UnwrapNamedType(UnwrapNullableType(colType))));
        ac.Nullable = IsNullableType(colType);
        if (ac.Nullable) {
            const std::string colName = "arg_column_" + std::to_string(idx);
            update.push_back(var(colName, columnValueType));
            update.push_back(assign(colName, columnAt(idx)));
            ac.Mat = BuildColumnValueAst(
                colName, "i", "arg_value_" + std::to_string(idx), colType, stringViewType);
        } else {
            auto valType = UnwrapNullableType(colType);
            auto ptrValType = std::make_shared<TPointerType>(valType);
            const std::string ptrName = "values_" + std::to_string(idx);
            update.push_back(var(ptrName, ptrValType));
            update.push_back(assign(ptrName, columnData(idx, ptrValType)));
        }
        argColumns.emplace(idx, std::move(ac));
    }
    update.push_back(var("selection_is_null", boolType));
    update.push_back(assign("selection_is_null",
        binary("==", cast(ident("selection"), i64Type), numI64(0))));
    update.push_back(var("dense_slot", i64Type));
    update.push_back(assign("dense_slot", numI64(-1)));
    update.push_back(var("is_new", i64Type));
    update.push_back(assign("is_new", numI64(0)));
    update.push_back(var("stored_witness", key.StoredType));
    update.push_back(assign("stored_witness", ZeroValueExpr(key.StoredType)));
    update.push_back(var("i", i64Type));
    update.push_back(assign("i", numI64(0)));

    auto selected = binary("||", ident("selection_is_null"),
        binary("!=", std::make_shared<TIndexExpr>(loc, ident("selection"), ident("i")),
            number(0, u8Type)));
    std::vector<TColumnValueAst> keyFields;
    keyFields.reserve(key.Fields.size());
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        auto logicalType = key.Fields[fieldIndex].IsNullable
            ? std::make_shared<TNullable>(key.Fields[fieldIndex].Type)
            : key.Fields[fieldIndex].Type;
        keyFields.push_back(BuildColumnValueAst(
            "key_column_" + std::to_string(fieldIndex), "i",
            "key_value_" + std::to_string(fieldIndex),
            std::move(logicalType), stringViewType));
    }

    std::vector<TExprPtr> fields;
    auto namedKey = TMaybeType<TNamedType>(key.LookupType);
    auto keyStruct = namedKey
        ? TMaybeType<TStructType>(namedKey.Cast()->UnderlyingType)
        : TMaybeType<TStructType>(key.LookupType);
    if (!keyStruct) {
        throw std::invalid_argument(
            "GenGenericAggregateDispatchAst: key must be a struct");
    }
    fields.reserve(keyStruct.Cast()->Fields.size());
    for (const auto& [fieldName, fieldType] : keyStruct.Cast()->Fields) {
        if (fieldName.starts_with("__qdb_padding_")) {
            fields.push_back(number(0, fieldType));
            continue;
        }
        const bool validity = fieldName.starts_with("valid_");
        const std::string_view prefix = validity ? "valid_" : "key_";
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
        fields.push_back(validity
            ? keyFields[fieldIndex].IsValid
            : keyFields[fieldIndex].Value);
    }
    TExprPtr keyValue = std::make_shared<TStructConstructExpr>(
        loc, key.LookupType, std::move(fields));
    auto upsertCall = call("aht_upsert_dual", {
        ident("ht"),
        std::move(keyValue),
        ident("stored_witness"),
        ident("is_new"),
    });
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);
    auto slotIndex = [&](const std::string& buf) -> TExprPtr {
        return std::make_shared<TIndexExpr>(loc, ident(buf), ident("dense_slot"));
    };

    std::vector<TExprPtr> materialize;
    for (auto& keyField : keyFields) {
        materialize.insert(materialize.end(),
            std::make_move_iterator(keyField.Setup.begin()),
            std::make_move_iterator(keyField.Setup.end()));
    }
    // Compute each arg column's value (as i64 bits) and validity once per row.
    for (auto& [idx, ac] : argColumns) {
        const std::string vname = "arg_val_" + std::to_string(idx);
        TExprPtr valExpr;
        if (ac.Nullable) {
            materialize.insert(materialize.end(),
                std::make_move_iterator(ac.Mat->Setup.begin()),
                std::make_move_iterator(ac.Mat->Setup.end()));
            valExpr = cast(ac.Mat->Value, i64Type); // nullable f64 disallowed upstream
        } else {
            auto cell = std::make_shared<TIndexExpr>(loc,
                ident("values_" + std::to_string(idx)), ident("i"));
            valExpr = ac.Float
                ? bitcast(std::move(cell), i64Type)
                : cast(std::move(cell), i64Type);
        }
        materialize.push_back(var(vname, i64Type));
        materialize.push_back(assign(vname, std::move(valExpr)));
        if (ac.Nullable) {
            const std::string validName = "arg_valid_" + std::to_string(idx);
            materialize.push_back(var(validName, boolType));
            materialize.push_back(assign(validName, ac.Mat->IsValid));
        }
    }

    // Inlined per-reducer applications (each reads its own column's value).
    std::vector<TExprPtr> reducerStmts;
    reducerStmts.push_back(var("agg_buffers", ptrPtrI64Type));
    reducerStmts.push_back(assign("agg_buffers", field("ht", "AggBuffers")));
    for (size_t ri = 0; ri < layout.Reducers.size(); ++ri) {
        const auto& info = layout.Reducers[ri];
        const std::string bufName = "buf_" + std::to_string(ri);
        const std::string reduceName = "reduce_" + std::to_string(ri);
        reducerStmts.push_back(var(bufName, ptrI64Type));
        reducerStmts.push_back(assign(bufName,
            std::make_shared<TIndexExpr>(loc, ident("agg_buffers"),
                numI64(static_cast<int64_t>(ri)))));
        auto valueI = [&]() -> TExprPtr {
            return info.HasArg ? ident("arg_val_" + std::to_string(info.ArgColumnIndex))
                               : numI64(0);
        };
        auto validI = [&]() -> TExprPtr {
            return info.NeedsValidity
                ? ident("arg_valid_" + std::to_string(info.ArgColumnIndex))
                : number(1, boolType);
        };
        if (!info.NeedsValidity) {
            auto callR = call(reduceName, {slotIndex(bufName), valueI(),
                binary("!=", ident("is_new"), numI64(0))});
            reducerStmts.push_back(std::make_shared<TArrayAssignExpr>(loc, bufName,
                std::vector<TExprPtr>{ident("dense_slot")}, std::move(callR)));
        } else if (info.Func == "count") {
            reducerStmts.push_back(call(reduceName,
                {ident(bufName), ident("dense_slot"), validI()}));
        } else {
            const std::string validBufName = "validbuf_" + std::to_string(ri);
            reducerStmts.push_back(var(validBufName, ptrI64Type));
            reducerStmts.push_back(assign(validBufName,
                std::make_shared<TIndexExpr>(loc, ident("agg_buffers"),
                    numI64(static_cast<int64_t>(info.ValidBufIdx)))));
            reducerStmts.push_back(call(reduceName, {ident(bufName),
                ident(validBufName), ident("dense_slot"), valueI(), validI()}));
        }
    }

    std::vector<TExprPtr> validBody = {
        assign("dense_slot", std::move(upsertCall)),
        std::make_shared<TIfExpr>(loc,
            binary("<", ident("dense_slot"), numI64(0)),
            block({std::make_shared<TReturnExpr>(loc, numI64(-1))}), nullptr),
    };
    validBody.insert(validBody.end(),
        std::make_move_iterator(reducerStmts.begin()),
        std::make_move_iterator(reducerStmts.end()));
    materialize.push_back(block(std::move(validBody)));
    auto process = block(std::move(materialize));
    auto loop = block({
        std::make_shared<TIfExpr>(loc, std::move(selected), std::move(process), nullptr),
        assign("i", binary("+", ident("i"), numI64(1))),
    });
    update.push_back(std::make_shared<TWhileStmtExpr>(
        loc, binary("<", ident("i"), ident("n")), std::move(loop)));
    update.push_back(numI64(0));

    auto init = cast(call("aht_init", {
        ident("ht"), ident("arg"),
        numI64(static_cast<int64_t>(layout.NumAggBuffers)),
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
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr columnType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto u8Type = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto ptrKeyType = std::make_shared<TPointerType>(key.KeyType);
    auto ptrU8Type = std::make_shared<TPointerType>(u8Type);
    auto ptrPtrU8Type = std::make_shared<TPointerType>(ptrU8Type);

    auto numI64 = [&](int64_t value) -> TExprPtr {
        auto expr = std::make_shared<TNumberExpr>(loc, value);
        expr->Type = i64Type;
        return expr;
    };
    const bool anyNullableAgg = std::any_of(
        layout.Reducers.begin(), layout.Reducers.end(),
        [](const TAggReducerInfo& r) { return r.IsNullableOutput; });
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);
    auto ptrColumnType = columnType
        ? std::make_shared<TPointerType>(
            std::make_shared<TNamedType>("TColumn", columnType))
        : nullptr;
    auto hashTableRefType = std::make_shared<TReferenceType>(
        std::make_shared<TNamedType>("HashTable", std::move(hashTableType)));
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "ht", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "output_key_buffers", ptrPtrU8Type),
        std::make_shared<TVarStmt>(loc, "output_buffers", ptrPtrI64Type),
        std::make_shared<TVarStmt>(loc, "output_agg_masks", ptrPtrU8Type),
        std::make_shared<TVarStmt>(loc, "output_capacity", i64Type),
    };
    // Only the first layout.Reducers.size() AggBuffers are value buffers exposed
    // to output_buffers; trailing valid-count buffers stay internal.
    auto stateCall = std::make_shared<TCallExpr>(loc, ident("aht_finalize_states"),
        std::vector<TExprPtr>{
            ident("ht"), ident("output_buffers"),
            numI64(static_cast<int64_t>(layout.Reducers.size())),
            ident("output_capacity"),
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
    if (!ptrColumnType) {
        throw std::invalid_argument(
            "GenGenericAggregateFinalizeAst: missing TColumn type");
    }
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        const bool isString = TMaybeType<TStringType>(
            UnwrapNamedType(key.Fields[fieldIndex].Type));
        const std::string columnName =
            "output_column_" + std::to_string(fieldIndex);
        const std::string maskName =
            "output_mask_" + std::to_string(fieldIndex);
        project.push_back(std::make_shared<TVarStmt>(
            loc, columnName, ptrColumnType));
        auto raw = std::make_shared<TIndexExpr>(loc,
            ident("output_key_buffers"),
            std::make_shared<TNumberExpr>(
                loc, static_cast<int64_t>(fieldIndex)));
        project.push_back(std::make_shared<TAssignExpr>(loc, columnName,
            std::make_shared<TCastExpr>(loc,
                std::make_shared<TCastExpr>(loc, std::move(raw), i64Type),
                ptrColumnType)));
        auto column = std::make_shared<TIndexExpr>(
            loc, ident(columnName), std::make_shared<TNumberExpr>(loc, int64_t{0}));
        if (key.Fields[fieldIndex].IsNullable) {
            project.push_back(std::make_shared<TVarStmt>(loc, maskName, ptrU8Type));
            project.push_back(std::make_shared<TAssignExpr>(loc, maskName,
                std::make_shared<TFieldAccessExpr>(loc, column, "Mask")));
        }
        if (isString) {
            const std::string dataName =
                "output_data_" + std::to_string(fieldIndex);
            const std::string offsetsName =
                "output_offsets_" + std::to_string(fieldIndex);
            const std::string copyResultName =
                "output_copy_result_" + std::to_string(fieldIndex);
            column = std::make_shared<TIndexExpr>(
                loc, ident(columnName), std::make_shared<TNumberExpr>(loc, int64_t{0}));
            project.push_back(std::make_shared<TVarStmt>(loc, dataName, ptrU8Type));
            project.push_back(std::make_shared<TAssignExpr>(loc, dataName,
                std::make_shared<TCastExpr>(loc,
                    std::make_shared<TCastExpr>(loc,
                        std::make_shared<TFieldAccessExpr>(
                            loc, column, "Data"), i64Type),
                    ptrU8Type)));
            column = std::make_shared<TIndexExpr>(
                loc, ident(columnName), std::make_shared<TNumberExpr>(loc, int64_t{0}));
            project.push_back(std::make_shared<TVarStmt>(
                loc, offsetsName, ptrI64Type));
            project.push_back(std::make_shared<TAssignExpr>(loc, offsetsName,
                std::make_shared<TCastExpr>(loc,
                    std::make_shared<TCastExpr>(loc,
                        std::make_shared<TFieldAccessExpr>(
                            loc, column, "Offsets"), i64Type),
                    ptrI64Type)));
            project.push_back(std::make_shared<TArrayAssignExpr>(loc,
                offsetsName,
                std::vector<TExprPtr>{
                    std::make_shared<TNumberExpr>(loc, int64_t{0})},
                std::make_shared<TNumberExpr>(loc, int64_t{0})));
            project.push_back(std::make_shared<TVarStmt>(
                loc, copyResultName, i64Type));
            project.push_back(std::make_shared<TAssignExpr>(
                loc, copyResultName,
                std::make_shared<TNumberExpr>(loc, int64_t{0})));
            continue;
        }
        const std::string name = "output_key_" + std::to_string(fieldIndex);
        auto ptrFieldType = std::make_shared<TPointerType>(key.Fields[fieldIndex].Type);
        project.push_back(std::make_shared<TVarStmt>(loc, name, ptrFieldType));
        column = std::make_shared<TIndexExpr>(
            loc, ident(columnName), std::make_shared<TNumberExpr>(loc, int64_t{0}));
        project.push_back(std::make_shared<TAssignExpr>(loc, name,
            std::make_shared<TCastExpr>(loc,
                std::make_shared<TCastExpr>(loc,
                    std::make_shared<TFieldAccessExpr>(
                        loc, column, "Data"), i64Type),
                ptrFieldType)));
    }
    if (anyNullableAgg) {
        // Bind the internal valid-count buffers so the slot loop can derive each
        // nullable aggregate's output mask (a group with zero valid arguments
        // produces a NULL result).
        project.push_back(std::make_shared<TVarStmt>(loc, "agg_buffers", ptrPtrI64Type));
        project.push_back(std::make_shared<TAssignExpr>(loc, "agg_buffers",
            std::make_shared<TFieldAccessExpr>(loc, ident("ht"), "AggBuffers")));
        for (size_t i = 0; i < layout.Reducers.size(); ++i) {
            const auto& r = layout.Reducers[i];
            if (!r.IsNullableOutput) {
                continue;
            }
            const std::string validName = "validbuf_" + std::to_string(i);
            project.push_back(std::make_shared<TVarStmt>(loc, validName, ptrI64Type));
            project.push_back(std::make_shared<TAssignExpr>(loc, validName,
                std::make_shared<TIndexExpr>(loc, ident("agg_buffers"),
                    numI64(static_cast<int64_t>(r.ValidBufIdx)))));
        }
    }
    project.push_back(std::make_shared<TVarStmt>(loc, "slot", i64Type));
    project.push_back(std::make_shared<TAssignExpr>(
        loc, "slot", std::make_shared<TNumberExpr>(loc, int64_t{0})));

    std::vector<TExprPtr> loopStmts;
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        auto keyValue = std::make_shared<TIndexExpr>(
            loc, ident("group_keys"), ident("slot"));
        TExprPtr value;
        value = std::make_shared<TFieldAccessExpr>(
            loc, keyValue, "key_" + std::to_string(fieldIndex));
        if (key.Fields[fieldIndex].IsNullable) {
            keyValue = std::make_shared<TIndexExpr>(
                loc, ident("group_keys"), ident("slot"));
            auto valid = std::make_shared<TFieldAccessExpr>(
                loc, keyValue, "valid_" + std::to_string(fieldIndex));
            loopStmts.push_back(std::make_shared<TCallExpr>(loc,
                ident("qdb_bitmap_set_valid"),
                std::vector<TExprPtr>{
                    ident("output_mask_" + std::to_string(fieldIndex)),
                    ident("slot"), std::move(valid)}));
        }
        const bool isString = TMaybeType<TStringType>(
            UnwrapNamedType(key.Fields[fieldIndex].Type));
        if (!isString) {
            loopStmts.push_back(std::make_shared<TArrayAssignExpr>(
                loc, "output_key_" + std::to_string(fieldIndex),
                std::vector<TExprPtr>{ident("slot")}, std::move(value)));
            continue;
        }
        const std::string dataName =
            "output_data_" + std::to_string(fieldIndex);
        const std::string offsetsName =
            "output_offsets_" + std::to_string(fieldIndex);
        const std::string copyResultName =
            "output_copy_result_" + std::to_string(fieldIndex);
        auto offset = std::make_shared<TIndexExpr>(
            loc, ident(offsetsName), ident("slot"));
        auto destination = std::make_shared<TCastExpr>(loc,
            std::make_shared<TBinaryExpr>(loc, TOperator("+"),
                std::make_shared<TCastExpr>(loc, ident(dataName), i64Type),
                offset), ptrU8Type);
        auto source = std::make_shared<TFieldAccessExpr>(
            loc, value, "Data");
        auto byteSize = std::make_shared<TFieldAccessExpr>(
            loc, value, "Size");
        loopStmts.push_back(std::make_shared<TAssignExpr>(loc, copyResultName,
            std::make_shared<TCallExpr>(loc,
                ident("qdb_string_copy_bytes"),
                std::vector<TExprPtr>{
                    std::move(destination), std::move(source), byteSize})));
        loopStmts.push_back(std::make_shared<TArrayAssignExpr>(loc,
            offsetsName,
            std::vector<TExprPtr>{std::make_shared<TBinaryExpr>(loc,
                TOperator("+"), ident("slot"),
                std::make_shared<TNumberExpr>(loc, int64_t{1}))},
            std::make_shared<TBinaryExpr>(loc, TOperator("+"),
                std::make_shared<TIndexExpr>(
                    loc, ident(offsetsName), ident("slot")),
                std::make_shared<TFieldAccessExpr>(loc, value, "Size"))));
    }
    for (size_t i = 0; i < layout.Reducers.size(); ++i) {
        const auto& r = layout.Reducers[i];
        if (!r.IsNullableOutput) {
            continue;
        }
        const std::string validName = "validbuf_" + std::to_string(i);
        auto validCount = std::make_shared<TIndexExpr>(
            loc, ident(validName), ident("slot"));
        loopStmts.push_back(std::make_shared<TCallExpr>(loc,
            ident("qdb_bitmap_set_valid"),
            std::vector<TExprPtr>{
                std::make_shared<TIndexExpr>(loc, ident("output_agg_masks"),
                    numI64(static_cast<int64_t>(i))),
                ident("slot"),
                std::make_shared<TBinaryExpr>(loc, TOperator(">"),
                    std::move(validCount), numI64(0))}));
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

NQumir::NAst::TExprPtr GenGenericAggregateMeasureAst(
    const TAggregateKeyDescriptor& key,
    NQumir::NAst::TTypePtr hashTableType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrKeyType = std::make_shared<TPointerType>(key.StoredType);
    auto hashTableRefType = std::make_shared<TReferenceType>(
        std::make_shared<TNamedType>("HashTable", std::move(hashTableType)));
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto number = [&](int64_t value) -> TExprPtr {
        auto result = std::make_shared<TNumberExpr>(loc, value);
        result->Type = i64Type;
        return result;
    };
    auto binary = [&](const char* op, TExprPtr left, TExprPtr right) -> TExprPtr {
        return std::make_shared<TBinaryExpr>(
            loc, TOperator(op), std::move(left), std::move(right));
    };
    auto outputAt = [&](size_t fieldIndex) -> TExprPtr {
        return std::make_shared<TIndexExpr>(loc, ident("output_key_bytes"),
            number(static_cast<int64_t>(fieldIndex)));
    };

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "ht", hashTableRefType),
        std::make_shared<TVarStmt>(loc, "output_key_bytes", ptrI64Type),
        std::make_shared<TVarStmt>(loc, "output_capacity", i64Type),
    };
    std::vector<TExprPtr> body;
    body.push_back(std::make_shared<TVarStmt>(loc, "size", i64Type));
    body.push_back(std::make_shared<TAssignExpr>(loc, "size",
        std::make_shared<TFieldAccessExpr>(loc, ident("ht"), "Size")));
    body.push_back(std::make_shared<TIfExpr>(loc,
        binary("<", ident("output_capacity"), ident("size")),
        std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{
            std::make_shared<TReturnExpr>(loc, number(-1))}), nullptr));
    body.push_back(std::make_shared<TVarStmt>(loc, "group_keys", ptrKeyType));
    body.push_back(std::make_shared<TAssignExpr>(loc, "group_keys",
        std::make_shared<TCastExpr>(loc,
            std::make_shared<TFieldAccessExpr>(loc, ident("ht"), "GroupKeys"),
            ptrKeyType)));

    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        const auto logical = UnwrapNamedType(key.Fields[fieldIndex].Type);
        if (ContainsLogicalString(key.Fields[fieldIndex].Type) &&
            !TMaybeType<TStringType>(logical)) {
            throw std::invalid_argument(
                "GenGenericAggregateMeasureAst: nested variable-width output "
                "is not supported");
        }
        if (TMaybeType<TStringType>(logical)) {
            body.push_back(std::make_shared<TArrayAssignExpr>(loc,
                "output_key_bytes", std::vector<TExprPtr>{
                    number(static_cast<int64_t>(fieldIndex))}, number(0)));
        } else {
            body.push_back(std::make_shared<TArrayAssignExpr>(loc,
                "output_key_bytes",
                std::vector<TExprPtr>{number(static_cast<int64_t>(fieldIndex))},
                binary("*", ident("size"),
                    number(static_cast<int64_t>(key.Fields[fieldIndex].Size)))));
        }
    }

    body.push_back(std::make_shared<TVarStmt>(loc, "slot", i64Type));
    body.push_back(std::make_shared<TAssignExpr>(loc, "slot", number(0)));
    std::vector<TExprPtr> loop;
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        if (!TMaybeType<TStringType>(UnwrapNamedType(key.Fields[fieldIndex].Type))) {
            continue;
        }
        TExprPtr stored = std::make_shared<TIndexExpr>(
            loc, ident("group_keys"), ident("slot"));
        stored = std::make_shared<TFieldAccessExpr>(
            loc, std::move(stored), "key_" + std::to_string(fieldIndex));
        auto size = std::make_shared<TFieldAccessExpr>(
            loc, std::move(stored), "Size");
        loop.push_back(std::make_shared<TArrayAssignExpr>(loc,
            "output_key_bytes",
            std::vector<TExprPtr>{number(static_cast<int64_t>(fieldIndex))},
            binary("+", outputAt(fieldIndex), std::move(size))));
    }
    loop.push_back(std::make_shared<TAssignExpr>(loc, "slot",
        binary("+", ident("slot"), number(1))));
    body.push_back(std::make_shared<TWhileStmtExpr>(loc,
        binary("<", ident("slot"), ident("size")),
        std::make_shared<TBlockExpr>(loc, std::move(loop))));
    body.push_back(std::make_shared<TReturnExpr>(loc, ident("size")));

    auto function = std::make_shared<TFunDecl>(loc, "agg_measure_keys",
        std::move(params), std::make_shared<TBlockExpr>(loc, std::move(body)),
        i64Type);
    return std::make_shared<TBlockExpr>(
        loc, std::vector<TExprPtr>{std::move(function)});
}

std::vector<NQumir::NAst::TExprPtr> GenReducerFunDecls(
    const TAggReducerLayout& layout)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto u64Type = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto f64Type = std::make_shared<TFloatType>();
    auto boolType = std::make_shared<TBoolType>();
    auto voidType = std::make_shared<TVoidType>();
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);

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
    auto block = [&](std::vector<TExprPtr> stmts) -> std::shared_ptr<TBlockExpr> {
        return std::make_shared<TBlockExpr>(loc, std::move(stmts));
    };
    // buf[dense_slot]
    auto slotOf = [&](const std::string& buf) -> TExprPtr {
        return std::make_shared<TIndexExpr>(loc, ident(buf), ident("dense_slot"));
    };
    // buf[dense_slot] = value
    auto storeSlot = [&](const std::string& buf, TExprPtr value) -> TExprPtr {
        return std::make_shared<TArrayAssignExpr>(loc, buf,
            std::vector<TExprPtr>{ident("dense_slot")}, std::move(value));
    };
    auto bitcast = [&](TExprPtr e, TTypePtr type) -> TExprPtr {
        return std::make_shared<TBitcastExpr>(loc,
            std::move(e),
            type);
    };

    std::vector<TExprPtr> result;
    result.reserve(layout.Reducers.size());
    for (size_t i = 0; i < layout.Reducers.size(); ++i) {
        const auto& r = layout.Reducers[i];
        const std::string& func = r.Func;
        const std::string name = "reduce_" + std::to_string(i);

        // Shape A: scalar accumulator over a single i64 buffer slot. Used for
        // the whole non-nullable-argument path and for count(*). Byte-identical
        // to the historical reducer contract.
        if (!r.NeedsValidity) {
            std::vector<TParam> params = {
                std::make_shared<TVarStmt>(loc, "prev", i64Type),
                std::make_shared<TVarStmt>(loc, "value", i64Type),
                std::make_shared<TVarStmt>(loc, "is_new", boolType),
            };
            TExprPtr resultExpr;
            if (r.IsFloat) {
                // f64 reducer: states/values are carried as i64 bits, so we
                // bitcast to f64, run f64 arithmetic, and bitcast back to i64.
                auto prevF = bitcast(ident("prev"), f64Type);
                auto valueF = bitcast(ident("value"), f64Type);
                if (func == "sum") {
                    resultExpr = bitcast(binary("+", prevF, valueF), i64Type);
                } else if (func == "min") {
                    resultExpr = bitcast(std::make_shared<TIfExpr>(loc, ident("is_new"),
                        valueF,
                        std::make_shared<TIfExpr>(loc, binary("<", valueF, prevF),
                            valueF, prevF)), i64Type);
                } else if (func == "max") {
                    resultExpr = bitcast(std::make_shared<TIfExpr>(loc, ident("is_new"),
                        valueF,
                        std::make_shared<TIfExpr>(loc, binary(">", valueF, prevF),
                            valueF, prevF)), i64Type);
                } else {
                    throw std::invalid_argument(
                        "GenReducerFunDecls: unsupported float aggregate: " + func);
                }
            } else if (func == "count") {
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
                throw std::invalid_argument(
                    "GenReducerFunDecls: unknown aggregate function: " + func);
            }
            auto body = block({std::make_shared<TReturnExpr>(loc, std::move(resultExpr))});
            result.push_back(std::make_shared<TFunDecl>(loc, name,
                std::move(params), std::move(body), i64Type));
            continue;
        }

        // Shape B-count: count(arg) with a nullable argument. Only counts rows
        // whose argument is non-null; count(*) never reaches here (HasArg false).
        if (func == "count") {
            std::vector<TParam> params = {
                std::make_shared<TVarStmt>(loc, "buf", ptrI64Type),
                std::make_shared<TVarStmt>(loc, "dense_slot", i64Type),
                std::make_shared<TVarStmt>(loc, "value_is_valid", boolType),
            };
            auto body = block({
                std::make_shared<TIfExpr>(loc, ident("value_is_valid"),
                    block({storeSlot("buf", binary("+", slotOf("buf"), numI64(1)))}),
                    nullptr),
            });
            result.push_back(std::make_shared<TFunDecl>(loc, name,
                std::move(params), std::move(body), voidType));
            continue;
        }

        // Shape B-agg: sum/min/max with a nullable argument. valid_buf tracks the
        // number of non-null contributions seen so far; the first valid value
        // seeds buf, and only later values accumulate/compare (so min/max never
        // fold against the zero-initialized buffer).
        std::vector<TParam> params = {
            std::make_shared<TVarStmt>(loc, "buf", ptrI64Type),
            std::make_shared<TVarStmt>(loc, "valid_buf", ptrI64Type),
            std::make_shared<TVarStmt>(loc, "dense_slot", i64Type),
            std::make_shared<TVarStmt>(loc, "value", i64Type),
            std::make_shared<TVarStmt>(loc, "value_is_valid", boolType),
        };
        TExprPtr accumulate;
        if (func == "sum") {
            accumulate = storeSlot("buf", binary("+", slotOf("buf"), ident("value")));
        } else if (func == "min") {
            accumulate = std::make_shared<TIfExpr>(loc,
                binary("<", ident("value"), slotOf("buf")),
                block({storeSlot("buf", ident("value"))}), nullptr);
        } else if (func == "max") {
            accumulate = std::make_shared<TIfExpr>(loc,
                binary(">", ident("value"), slotOf("buf")),
                block({storeSlot("buf", ident("value"))}), nullptr);
        } else {
            throw std::invalid_argument(
                "GenReducerFunDecls: unknown aggregate function: " + func);
        }
        auto seedOrAccumulate = std::make_shared<TIfExpr>(loc,
            binary("==", slotOf("valid_buf"), numI64(0)),
            block({storeSlot("buf", ident("value"))}),
            block({std::move(accumulate)}));
        auto inner = block({
            std::move(seedOrAccumulate),
            storeSlot("valid_buf", binary("+", slotOf("valid_buf"), numI64(1))),
        });
        auto body = block({
            std::make_shared<TIfExpr>(loc, ident("value_is_valid"),
                std::move(inner), nullptr),
        });
        result.push_back(std::make_shared<TFunDecl>(loc, name,
            std::move(params), std::move(body), voidType));
    }
    return result;
}

NQumir::NAst::TExprPtr GenApplyReducersFunDecl(const TAggReducerLayout& layout)
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
    auto bindBuffer = [&](const std::string& name, int index, std::vector<TExprPtr>& out) {
        out.push_back(std::make_shared<TVarStmt>(loc, name, ptrI64Type));
        out.push_back(std::make_shared<TAssignExpr>(loc, name,
            std::make_shared<TIndexExpr>(loc, ident("agg_buffers"),
                numI64(static_cast<int64_t>(index)))));
    };

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "agg_buffers", ptrPtrI64Type),
        std::make_shared<TVarStmt>(loc, "dense_slot", i64Type),
        std::make_shared<TVarStmt>(loc, "value", i64Type),
        std::make_shared<TVarStmt>(loc, "value_is_valid", boolType),
        std::make_shared<TVarStmt>(loc, "is_new", boolType),
    };

    std::vector<TExprPtr> stmts;
    for (size_t i = 0; i < layout.Reducers.size(); ++i) {
        const auto& r = layout.Reducers[i];
        const std::string bufName = "buf_" + std::to_string(i);
        const std::string reduceName = "reduce_" + std::to_string(i);
        bindBuffer(bufName, static_cast<int>(i), stmts);

        if (!r.NeedsValidity) {
            // Shape A: out-of-line scalar reducer; capture and store the result.
            auto prev = std::make_shared<TIndexExpr>(loc, ident(bufName), ident("dense_slot"));
            auto call = std::make_shared<TCallExpr>(loc, ident(reduceName),
                std::vector<TExprPtr>{prev, ident("value"), ident("is_new")});
            stmts.push_back(std::make_shared<TArrayAssignExpr>(loc, bufName,
                std::vector<TExprPtr>{ident("dense_slot")}, call));
            continue;
        }

        if (r.Func == "count") {
            // Shape B-count: in-place reducer, no return value.
            stmts.push_back(std::make_shared<TCallExpr>(loc, ident(reduceName),
                std::vector<TExprPtr>{
                    ident(bufName), ident("dense_slot"), ident("value_is_valid")}));
            continue;
        }

        // Shape B-agg: in-place reducer over value buffer + valid-count buffer.
        const std::string validName = "validbuf_" + std::to_string(i);
        bindBuffer(validName, r.ValidBufIdx, stmts);
        stmts.push_back(std::make_shared<TCallExpr>(loc, ident(reduceName),
            std::vector<TExprPtr>{
                ident(bufName), ident(validName), ident("dense_slot"),
                ident("value"), ident("value_is_valid")}));
    }

    auto body = std::make_shared<TBlockExpr>(loc, std::move(stmts));
    return std::make_shared<TFunDecl>(loc, "agg_apply_reducers",
        std::move(params), std::move(body), std::make_shared<TVoidType>());
}

} // namespace NKernel
} // namespace NQdb
