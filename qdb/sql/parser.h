#pragma once

#include "lexer.h"
#include "ast.h"

#include <expected>
#include <vector>

#include <qumir/error.h>

namespace NQdb {
namespace NSql {

class TParser {
public:
    std::expected<TSqlNodePtr, NQumir::TError> Parse(TTokenStream& stream);
    std::expected<std::vector<TSqlNodePtr>, NQumir::TError> ParseAll(
        TTokenStream& stream);
};

} // namespace NSql
} // namespace NQdb
