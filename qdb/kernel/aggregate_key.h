#pragma once

#include <qumir/parser/type.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace NQqb::NKernel {

// Byte layout of a key (component) type.
struct TKeyLayout {
    size_t Size = 0;
    size_t Alignment = 0;
};

// Lookup/stored representations of a single key column type, shared by the
// aggregation and join key descriptors. For strings the lookup type is a
// non-owning StringView and the stored type an OwnedString; for fixed-width
// and string-free composites Lookup == Stored == Inner.
struct TRepresentedKeyType {
    NQumir::NAst::TTypePtr Lookup;
    NQumir::NAst::TTypePtr Stored;
    TKeyLayout Layout;
    bool HasString = false;
    bool Nullable = false;
    NQumir::NAst::TTypePtr Inner; // type with the Nullable wrapper stripped
};

size_t AlignUp(size_t value, size_t alignment);

// Computes the lookup/stored representation + byte layout of one key column
// type (integer/f64/bool/string/composite). Throws NQumir::TError for
// unsupported types.
TRepresentedKeyType RepresentKeyType(const NQumir::NAst::TTypePtr& original);

struct TAggregateKeyField {
    std::string ColumnName;
    int32_t ColumnIndex = -1;
    NQumir::NAst::TTypePtr Type;
    NQumir::NAst::TTypePtr LookupType;
    NQumir::NAst::TTypePtr StoredType;
    bool IsNullable = false;
    size_t Offset = 0;
    size_t Size = 0;
    size_t Alignment = 0;
};

struct TAggregateKeyDescriptor {
    std::string TypeName;
    std::string LookupTypeName;
    std::string StoredTypeName;
    NQumir::NAst::TTypePtr KeyType;
    NQumir::NAst::TTypePtr LookupType;
    NQumir::NAst::TTypePtr StoredType;
    std::vector<TAggregateKeyField> Fields;
    size_t Size = 0;
    size_t Alignment = 0;

    bool IsScalar() const {
        return Fields.size() == 1;
    }

    bool HasDistinctLookupType() const {
        return LookupType != StoredType;
    }
};

// Builds lookup and stored key representations for generated aggregation kernels.
TAggregateKeyDescriptor BuildAggregateKeyDescriptor(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys);

} // namespace NQqb::NKernel
