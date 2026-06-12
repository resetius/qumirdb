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
    size_t Offset = 0;
    size_t Size = 0;
    size_t Alignment = 0;
};

struct TAggregateKeyDescriptor {
    std::string TypeName;
    NQumir::NAst::TTypePtr KeyType;
    std::vector<TAggregateKeyField> Fields;
    size_t Size = 0;
    size_t Alignment = 0;

    bool IsScalar() const {
        return Fields.size() == 1;
    }
};

// Builds the fixed-width key representation used by generated aggregation
// kernels. Unsupported or missing fields produce NQumir::TError.
TAggregateKeyDescriptor BuildAggregateKeyDescriptor(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<std::string>& groupKeys);

} // namespace NQqb::NKernel
