#pragma once

#include <qumir/parser/type.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace NQqb::NKernel {

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
