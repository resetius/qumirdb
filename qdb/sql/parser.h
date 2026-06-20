#include "lexer.h"
#include "ast.h"

#include <expected>

#include <qumir/error.h>

namespace NQdb {
namespace NSql {

class TParser {
public:
    std::expected<TSqlNodePtr, NQumir::TError> Parse(TTokenStream& stream);
};

} // namespace NSql
} // namespace NQdb
