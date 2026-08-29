#pragma once

#include <qdb/io/physical_row_id.h>

#include <cstdint>
#include <stdexcept>

namespace NQdb {

inline constexpr uint64_t ParquetRowOffsetBits = 32;
inline constexpr uint64_t ParquetRowOffsetMask = (uint64_t{1} << ParquetRowOffsetBits) - 1;

inline TPhysicalRowId MakeParquetRowIdUnchecked(uint32_t rowGroup, uint32_t rowOffset)
{
    return (static_cast<uint64_t>(rowGroup) << ParquetRowOffsetBits) | static_cast<uint64_t>(rowOffset);
}

inline TPhysicalRowId MakeParquetRowId(uint64_t rowGroup, uint64_t rowOffset)
{
    if (rowGroup > ParquetRowOffsetMask || rowOffset > ParquetRowOffsetMask) {
        throw std::out_of_range("parquet row locator field exceeds 32 bits");
    }
    return MakeParquetRowIdUnchecked(static_cast<uint32_t>(rowGroup), static_cast<uint32_t>(rowOffset));
}

inline uint32_t ParquetRowGroup(TPhysicalRowId rowId) {
    return static_cast<uint32_t>(rowId >> ParquetRowOffsetBits);
}

inline uint32_t ParquetRowOffset(TPhysicalRowId rowId) {
    return static_cast<uint32_t>(rowId & ParquetRowOffsetMask);
}

} // namespace NQdb
