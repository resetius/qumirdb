#include <qdb/kernel/gen.h>

#include <qdb/kernel/column_value.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/operator.h>
#include <qumir/location.h>

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace NQqb {
namespace NKernel {

namespace {

enum class EFilterValueKind {
    Other,
    String,
};

EFilterValueKind SpecializeFilterPredicate(
    NQumir::NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& stringFields,
    const std::unordered_map<std::string, std::string>& stringValues,
    const NQumir::NAst::TTypePtr& stringViewType,
    std::vector<std::shared_ptr<std::string>>& literalStorage)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};
    if (auto ident = TMaybeNode<TIdentExpr>(expr)) {
        if (!stringFields.contains(ident.Cast()->Name)) {
            return EFilterValueKind::Other;
        }
        expr = std::make_shared<TIdentExpr>(
            expr->Location, stringValues.at(ident.Cast()->Name));
        return EFilterValueKind::String;
    }
    if (auto literal = TMaybeNode<TStringLiteralExpr>(expr)) {
        auto storage = std::make_shared<std::string>(literal.Cast()->Value);
        auto i64Type = std::make_shared<TIntegerType>();
        auto ptrU8Type = std::make_shared<TPointerType>(
            std::make_shared<TIntegerType>(TIntegerType::U8));
        auto address = std::make_shared<TNumberExpr>(loc,
            static_cast<int64_t>(reinterpret_cast<intptr_t>(storage->data())));
        address->Type = i64Type;
        auto data = std::make_shared<TCastExpr>(loc, std::move(address), ptrU8Type);
        auto size = std::make_shared<TNumberExpr>(loc,
            static_cast<int64_t>(storage->size()));
        size->Type = i64Type;
        expr = std::make_shared<TStructConstructExpr>(loc,
            std::make_shared<TNamedType>("StringView", stringViewType),
            std::vector<TExprPtr>{std::move(data), std::move(size)});
        literalStorage.push_back(std::move(storage));
        return EFilterValueKind::String;
    }
    if (auto cast = TMaybeNode<TCastExpr>(expr)) {
        if (SpecializeFilterPredicate(
                cast.Cast()->Operand, stringFields, stringValues,
                stringViewType, literalStorage) == EFilterValueKind::String) {
            expr = std::move(cast.Cast()->Operand);
            return EFilterValueKind::String;
        }
        return EFilterValueKind::Other;
    }
    if (auto binary = TMaybeNode<TBinaryExpr>(expr)) {
        const auto leftKind = SpecializeFilterPredicate(
            binary.Cast()->Left, stringFields, stringValues, stringViewType,
            literalStorage);
        const auto rightKind = SpecializeFilterPredicate(
            binary.Cast()->Right, stringFields, stringValues, stringViewType,
            literalStorage);
        if (leftKind == EFilterValueKind::String ||
            rightKind == EFilterValueKind::String) {
            if (leftKind != EFilterValueKind::String ||
                rightKind != EFilterValueKind::String) {
                throw std::invalid_argument(
                    "string filter comparison requires two StringView operands");
            }
            const auto op = binary.Cast()->Operator;
            if (!(op == "==" || op == "!=" || op == "<" || op == "<=" ||
                  op == ">" || op == ">=")) {
                throw std::invalid_argument(
                    "unsupported StringView filter operator '" + op.ToString() + "'");
            }
            auto compare = std::make_shared<TCallExpr>(loc,
                std::make_shared<TIdentExpr>(loc, "qdb_filter_string_compare"),
                std::vector<TExprPtr>{
                    std::make_shared<TFieldAccessExpr>(
                        loc, binary.Cast()->Left, "Data"),
                    std::make_shared<TFieldAccessExpr>(
                        loc, binary.Cast()->Left, "Size"),
                    std::make_shared<TFieldAccessExpr>(
                        loc, binary.Cast()->Right, "Data"),
                    std::make_shared<TFieldAccessExpr>(
                        loc, binary.Cast()->Right, "Size")});
            expr = std::make_shared<TBinaryExpr>(loc, op, std::move(compare),
                std::make_shared<TNumberExpr>(loc, int64_t{0}));
        }
        return EFilterValueKind::Other;
    }
    for (auto* child : expr->MutableChildren()) {
        if (SpecializeFilterPredicate(
                *child, stringFields, stringValues, stringViewType,
                literalStorage) ==
            EFilterValueKind::String) {
            throw std::invalid_argument(
                "StringView filter value must be an operand of a comparison");
        }
    }
    return EFilterValueKind::Other;
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
            if (fieldName.starts_with("key_")) {
                auto validPath = path;
                validPath.push_back(
                    "valid_" + fieldName.substr(std::string("key_").size()));
                fieldHash = std::make_shared<TIfExpr>(loc,
                    KeyValueExpr(root, validPath), std::move(fieldHash), number(0));
            }
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
            if (fieldName.starts_with("key_")) {
                auto validPath = path;
                validPath.push_back(
                    "valid_" + fieldName.substr(std::string("key_").size()));
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
            if (fieldName.starts_with("key_")) {
                auto validPath = path;
                validPath.push_back(
                    "valid_" + fieldName.substr(std::string("key_").size()));
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
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType,
    std::vector<std::shared_ptr<std::string>>& literalStorage)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    std::unordered_set<std::string> fixedFields;
    std::unordered_set<std::string> stringFields;
    std::unordered_map<std::string, std::string> stringValues;
    for (const auto& [name, type] : inputType.Fields) {
        if (TMaybeType<TStringType>(UnwrapNamedType(type))) {
            stringFields.insert(name);
            stringValues.emplace(name, name + "_value");
        } else {
            fixedFields.insert(name);
        }
    }

    auto identI = std::make_shared<TIdentExpr>(loc, "i");
    SpecializeFilterPredicate(
        predicate, stringFields, stringValues, stringViewType, literalStorage);
    SubstFieldsInPlace(predicate, fixedFields, identI);

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

    // Bind fixed-width columns as typed arrays and strings as TColumn values.
    auto i64Type = std::make_shared<TIntegerType>();
    std::vector<TExprPtr> loopSetup;
    for (const auto& [name, type] : inputType.Fields) {
        int32_t idx = fieldIndices.at(name);
        auto colElem = std::make_shared<TIndexExpr>(loc,
            std::make_shared<TIdentExpr>(loc, "cols"),
            std::make_shared<TNumberExpr>(loc, int64_t(idx)));
        if (TMaybeType<TStringType>(UnwrapNamedType(type))) {
            bodyStmts.push_back(std::make_shared<TVarStmt>(loc, name, columnValueType));
            bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, name, colElem));
            auto materialized = BuildColumnValueAst(
                name, "i", name + "_filter", type, stringViewType);
            loopSetup.insert(loopSetup.end(),
                std::make_move_iterator(materialized.Setup.begin()),
                std::make_move_iterator(materialized.Setup.end()));
            loopSetup.push_back(std::make_shared<TVarStmt>(
                loc, stringValues.at(name), materialized.ValueType));
            loopSetup.push_back(std::make_shared<TAssignExpr>(
                loc, stringValues.at(name), std::move(materialized.Value)));
            continue;
        }
        auto ptrFieldType = std::make_shared<TPointerType>(type);
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
    loopSetup.push_back(std::move(writeSel));
    loopSetup.push_back(std::move(incrI));
    bodyStmts.push_back(std::make_shared<TWhileStmtExpr>(loc, cond,
        std::make_shared<TBlockExpr>(loc, std::move(loopSetup))));

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
    TExprPtr value = argField
        ? cast(std::make_shared<TIndexExpr>(loc, ident("values"), ident("i")), i64Type)
        : numI64(0);
    std::vector<TColumnValueAst> keyFields;
    keyFields.reserve(key.Fields.size());
    auto stringViewType = FindStringViewType(key.LookupType);
    for (size_t fieldIndex = 0; fieldIndex < key.Fields.size(); ++fieldIndex) {
        keyFields.push_back(BuildColumnValueAst(
            "key_column_" + std::to_string(fieldIndex), "i",
            "key_value_" + std::to_string(fieldIndex),
            key.Fields[fieldIndex].Type, stringViewType));
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
    std::vector<TExprPtr> materialize;
    for (auto& keyField : keyFields) {
        materialize.insert(materialize.end(),
            std::make_move_iterator(keyField.Setup.begin()),
            std::make_move_iterator(keyField.Setup.end()));
    }
    auto reduceCall = call("agg_apply_reducers", {
        field("ht", "AggBuffers"), ident("dense_slot"), std::move(value),
        binary("!=", ident("is_new"), numI64(0)),
    });
    auto validProcess = block({
        assign("dense_slot", std::move(upsertCall)),
        std::make_shared<TIfExpr>(loc,
            binary("<", ident("dense_slot"), numI64(0)),
            block({std::make_shared<TReturnExpr>(loc, numI64(-1))}), nullptr),
        std::move(reduceCall),
    });
    materialize.push_back(std::move(validProcess));
    auto process = block(std::move(materialize));
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
        project.push_back(std::make_shared<TVarStmt>(loc, maskName, ptrU8Type));
        project.push_back(std::make_shared<TAssignExpr>(loc, maskName,
            std::make_shared<TFieldAccessExpr>(loc, column, "Mask")));
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
        keyValue = std::make_shared<TIndexExpr>(
            loc, ident("group_keys"), ident("slot"));
        auto valid = std::make_shared<TFieldAccessExpr>(
            loc, keyValue, "valid_" + std::to_string(fieldIndex));
        loopStmts.push_back(std::make_shared<TCallExpr>(loc,
            ident("qdb_bitmap_set_valid"),
            std::vector<TExprPtr>{
                ident("output_mask_" + std::to_string(fieldIndex)),
                ident("slot"), std::move(valid)}));
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
