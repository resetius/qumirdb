#pragma once

#include <span>
#include <string_view>

#include <qumir/parser/type.h>

namespace NQdb {

struct TColumnSchema {
    std::string_view Name;
    NQumir::NAst::TTypePtr Type;
};

struct TSchema {
    std::span<const TColumnSchema> Columns;
};

} // namespace NQdb
