#include <qdb/kernel/aggregate_key.h>

#include <qumir/error.h>

#include <algorithm>
#include <cctype>

namespace NQqb::NKernel {

namespace {

struct TLayout {
    size_t Size;
    size_t Alignment;
};

struct TRepresentedType {
    NQumir::NAst::TTypePtr Lookup;
    NQumir::NAst::TTypePtr Stored;
    TLayout Layout;
    bool HasString = false;
};

size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

TLayout TypeLayout(const NQumir::NAst::TTypePtr& original) {
    using namespace NQumir::NAst;

    auto type = UnwrapNamedType(original);
    if (auto integer = TMaybeType<TIntegerType>(type)) {
        const size_t size = static_cast<size_t>(integer.Cast()->BitWidth() / 8);
        return {size, std::min<size_t>(size, 8)};
    }
    if (TMaybeType<TFloatType>(type)) {
        return {8, 8};
    }
    if (TMaybeType<TBoolType>(type)) {
        return {1, 1};
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        size_t offset = 0;
        size_t maxAlignment = 1;
        for (const auto& [_, fieldType] : structure.Cast()->Fields) {
            const auto field = TypeLayout(fieldType);
            offset = AlignUp(offset, field.Alignment);
            offset += field.Size;
            maxAlignment = std::max(maxAlignment, field.Alignment);
        }
        const size_t alignment = std::min<size_t>(maxAlignment, 8);
        return {AlignUp(offset, alignment), alignment};
    }
    throw NQumir::TError(
        "aggregation key type is not fixed-width and trivially copyable: " +
        (original ? original->ToString() : std::string("<null>")));
}

NQumir::NAst::TTypePtr StringHandleType(const std::string& name) {
    using namespace NQumir::NAst;
    auto u8 = std::make_shared<TIntegerType>(TIntegerType::U8);
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto structure = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"Data", std::make_shared<TPointerType>(u8)},
            {"Size", i64},
        });
    return std::make_shared<TNamedType>(name, std::move(structure));
}

TRepresentedType RepresentKeyType(const NQumir::NAst::TTypePtr& original) {
    using namespace NQumir::NAst;

    auto type = UnwrapNamedType(original);
    if (TMaybeType<TStringType>(type)) {
        return {
            .Lookup = StringHandleType("StringView"),
            .Stored = StringHandleType("OwnedString"),
            .Layout = {16, 8},
            .HasString = true,
        };
    }
    if (TMaybeType<TIntegerType>(type) || TMaybeType<TFloatType>(type) ||
        TMaybeType<TBoolType>(type)) {
        return {
            .Lookup = original,
            .Stored = original,
            .Layout = TypeLayout(original),
        };
    }
    if (auto structure = TMaybeType<TStructType>(type)) {
        std::vector<std::pair<std::string, TTypePtr>> lookupFields;
        std::vector<std::pair<std::string, TTypePtr>> storedFields;
        size_t offset = 0;
        size_t maxAlignment = 1;
        size_t paddingIndex = 0;
        bool hasString = false;

        auto addPadding = [&](size_t targetOffset) {
            while (offset < targetOffset) {
                const std::string name =
                    "__qdb_padding_" + std::to_string(paddingIndex++);
                auto paddingType = std::make_shared<TIntegerType>(TIntegerType::U8);
                lookupFields.emplace_back(name, paddingType);
                storedFields.emplace_back(name, paddingType);
                ++offset;
            }
        };

        for (const auto& [name, fieldType] : structure.Cast()->Fields) {
            auto represented = RepresentKeyType(fieldType);
            addPadding(AlignUp(offset, represented.Layout.Alignment));
            lookupFields.emplace_back(name, represented.Lookup);
            storedFields.emplace_back(name, represented.Stored);
            offset += represented.Layout.Size;
            maxAlignment = std::max(maxAlignment, represented.Layout.Alignment);
            hasString = hasString || represented.HasString;
        }
        const size_t alignment = std::min<size_t>(maxAlignment, 8);
        const size_t size = AlignUp(offset, alignment);
        addPadding(size);
        if (!hasString) {
            return {
                .Lookup = original,
                .Stored = original,
                .Layout = {size, alignment},
            };
        }
        return {
            .Lookup = std::make_shared<TStructType>(std::move(lookupFields)),
            .Stored = std::make_shared<TStructType>(std::move(storedFields)),
            .Layout = {size, alignment},
            .HasString = hasString,
        };
    }
    throw NQumir::TError(
        "aggregation key type is not supported: " +
        (original ? original->ToString() : std::string("<null>")));
}

std::string KeyTypeName(const std::vector<TAggregateKeyField>& fields) {
    std::string name = "AggKey";
    for (const auto& field : fields) {
        name += "_" + field.ColumnName + "_" + field.Type->ToString();
    }
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            c = '_';
        }
    }
    return name;
}

} // namespace

TAggregateKeyDescriptor BuildAggregateKeyDescriptor(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys)
{
    using namespace NQumir::NAst;

    if (groupKeys.empty()) {
        throw NQumir::TError("aggregation key descriptor requires at least one group key");
    }

    TAggregateKeyDescriptor result;
    size_t offset = 0;
    size_t maxAlignment = 1;
    std::vector<std::pair<std::string, TTypePtr>> structFields;
    size_t paddingIndex = 0;

    auto addPadding = [&](size_t targetOffset) {
        while (offset < targetOffset) {
            structFields.emplace_back(
                "__qdb_padding_" + std::to_string(paddingIndex++),
                std::make_shared<TIntegerType>(TIntegerType::U8));
            ++offset;
        }
    };

    for (const auto& key : groupKeys) {
        int32_t columnIndex = -1;
        TTypePtr type;
        for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
            if (inputType.Fields[i].first == key) {
                columnIndex = i;
                type = inputType.Fields[i].second;
                break;
            }
        }
        if (!type) {
            throw NQumir::TError("aggregation key descriptor: unknown column '" + key + "'");
        }

        const auto represented = RepresentKeyType(type);
        const auto layout = represented.Layout;
        addPadding(AlignUp(offset, layout.Alignment));
        result.Fields.push_back({
            .ColumnName = key,
            .ColumnIndex = columnIndex,
            .Type = type,
            .LookupType = represented.Lookup,
            .StoredType = represented.Stored,
            .Offset = offset,
            .Size = layout.Size,
            .Alignment = layout.Alignment,
        });
        structFields.emplace_back(
            "key_" + std::to_string(result.Fields.size() - 1), represented.Stored);
        offset += layout.Size;
        maxAlignment = std::max(maxAlignment, layout.Alignment);
    }

    result.Alignment = std::min<size_t>(maxAlignment, 8);
    result.Size = AlignUp(offset, result.Alignment);
    if (!result.IsScalar()) {
        addPadding(result.Size);
    }
    result.TypeName = KeyTypeName(result.Fields);
    if (result.IsScalar()) {
        result.LookupType = result.Fields.front().LookupType;
        result.StoredType = result.Fields.front().StoredType;
        if (result.LookupType != result.StoredType &&
            TMaybeType<TStructType>(result.LookupType) &&
            TMaybeType<TStructType>(result.StoredType)) {
            result.LookupTypeName = result.TypeName + "_Lookup";
            result.StoredTypeName = result.TypeName + "_Stored";
            result.LookupType = std::make_shared<TNamedType>(
                result.LookupTypeName, result.LookupType);
            result.StoredType = std::make_shared<TNamedType>(
                result.StoredTypeName, result.StoredType);
            result.Fields.front().LookupType = result.LookupType;
            result.Fields.front().StoredType = result.StoredType;
        }
    } else {
        std::vector<std::pair<std::string, TTypePtr>> lookupFields;
        lookupFields.reserve(structFields.size());
        size_t keyIndex = 0;
        for (const auto& [name, storedType] : structFields) {
            if (name.starts_with("__qdb_padding_")) {
                lookupFields.emplace_back(name, storedType);
            } else {
                lookupFields.emplace_back(name, result.Fields[keyIndex++].LookupType);
            }
        }
        const bool hasDistinctType = std::any_of(
            result.Fields.begin(), result.Fields.end(),
            [](const auto& field) { return field.LookupType != field.StoredType; });
        if (hasDistinctType) {
            result.LookupTypeName = result.TypeName + "_Lookup";
            result.StoredTypeName = result.TypeName + "_Stored";
            result.LookupType = std::make_shared<TNamedType>(result.LookupTypeName,
                std::make_shared<TStructType>(std::move(lookupFields)));
            result.StoredType = std::make_shared<TNamedType>(result.StoredTypeName,
                std::make_shared<TStructType>(std::move(structFields)));
        } else {
            auto concrete = std::make_shared<TNamedType>(result.TypeName,
                std::make_shared<TStructType>(std::move(structFields)));
            result.LookupType = concrete;
            result.StoredType = concrete;
        }
    }
    result.KeyType = result.StoredType;
    return result;
}

} // namespace NQqb::NKernel
