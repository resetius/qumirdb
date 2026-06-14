#pragma once

#include <qumir/parser/type.h>

#include <memory>
#include <string>
#include <utility>

namespace NQqb {

struct TNullable : NQumir::NAst::TType {
    static constexpr const char* TypeId = "Nullable";

    NQumir::NAst::TTypePtr UnderlyingType;

    explicit TNullable(NQumir::NAst::TTypePtr underlyingType)
        : UnderlyingType(std::move(underlyingType))
    { }

    std::string ToString() const override {
        return "Nullable<" +
            (UnderlyingType ? UnderlyingType->ToString() : std::string("unknown")) +
            ">";
    }

    const std::string_view TypeName() const override {
        return TypeId;
    }
};

inline bool IsNullableType(const NQumir::NAst::TTypePtr& type) {
    return static_cast<bool>(NQumir::NAst::TMaybeType<TNullable>(type));
}

inline NQumir::NAst::TTypePtr UnwrapNullableType(
    const NQumir::NAst::TTypePtr& type)
{
    if (auto nullable = NQumir::NAst::TMaybeType<TNullable>(type)) {
        return nullable.Cast()->UnderlyingType;
    }
    return type;
}

} // namespace NQqb
