#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace NQdb {

struct TStringView {
    uint8_t* Data = nullptr;
    int64_t Size = 0;
};

struct TOwnedString {
    uint8_t* Data = nullptr;
    int64_t Size = 0;
};

static_assert(std::is_trivially_copyable_v<TStringView>);
static_assert(std::is_standard_layout_v<TStringView>);
static_assert(sizeof(TStringView) == 16);
static_assert(alignof(TStringView) == 8);
static_assert(offsetof(TStringView, Data) == 0);
static_assert(offsetof(TStringView, Size) == 8);

static_assert(std::is_trivially_copyable_v<TOwnedString>);
static_assert(std::is_standard_layout_v<TOwnedString>);
static_assert(sizeof(TOwnedString) == 16);
static_assert(alignof(TOwnedString) == 8);
static_assert(offsetof(TOwnedString, Data) == 0);
static_assert(offsetof(TOwnedString, Size) == 8);

} // namespace NQdb
