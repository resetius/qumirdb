#pragma once

#include <string>
#include <string_view>

namespace NQdb::NUtils {

std::string Sha1Hex(std::string_view data);

} // namespace NQdb::NUtils
