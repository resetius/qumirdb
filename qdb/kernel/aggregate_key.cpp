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

        const auto layout = TypeLayout(type);
        addPadding(AlignUp(offset, layout.Alignment));
        result.Fields.push_back({
            .ColumnName = key,
            .ColumnIndex = columnIndex,
            .Type = type,
            .Offset = offset,
            .Size = layout.Size,
            .Alignment = layout.Alignment,
        });
        structFields.emplace_back("key_" + std::to_string(result.Fields.size() - 1), type);
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
        result.KeyType = result.Fields.front().Type;
    } else {
        auto structure = std::make_shared<TStructType>(std::move(structFields));
        result.KeyType = std::make_shared<TNamedType>(result.TypeName, std::move(structure));
    }
    return result;
}

} // namespace NQqb::NKernel
