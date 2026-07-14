#pragma once

#include <qdb/plan/types/nullable.h>

#include <qumir/parser/type.h>

namespace NQdb {

// How a column's scalar stats (min/max/histogram) are stored in the 8-byte
// TColumnStats slots, decided by the column type. Shared by the parquet footer,
// the service stats JSON emitter, and the browser-dataset reader so the encoding
// stays in one place.
enum class EStatsScalarKind {
    None,  // string columns carry no scalar stats
    Int,   // std::bit_cast<int64_t>
    Float, // std::bit_cast<double>
};

inline EStatsScalarKind StatsScalarKind(const NQumir::NAst::TTypePtr& type) {
    using namespace NQumir::NAst;
    auto inner = UnwrapNullableType(type);
    if (TMaybeType<TStringType>(inner)) {
        return EStatsScalarKind::None;
    }
    if (TMaybeType<TFloatType>(inner)) {
        return EStatsScalarKind::Float;
    }
    return EStatsScalarKind::Int;
}

} // namespace NQdb
