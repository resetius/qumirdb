#pragma once

#include <span>
#include <string_view>

#include <qumir/parser/type.h>

namespace NQqb {

struct TColumnSchema {
    std::string_view Name;
    NQumir::NAst::TTypePtr Type;
};

struct TSchema {
    std::span<const TColumnSchema> Columns;
};

} // namespace NQqb
