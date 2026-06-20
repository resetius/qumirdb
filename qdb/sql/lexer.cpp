#include "lexer.h"

#include <unordered_set>

#include <qumir/parser/operator.h>

namespace NQqb {
namespace NSql {

using namespace NQumir;
using namespace NQumir::NAst;

namespace {

static const std::unordered_set<std::string> TwoCharOperators = {
    "<=", ">=", "<>", "!=", "&&", "||", "::"
};

static const std::unordered_set<int> TwoCharOperatorFirstChars = {
    '<', '>', '!', '|', '&', ':'
};

static const std::unordered_set<int> SingleCharOperators = {
    '(', ')', ',', ';', '=', '+', '-', '*', '/', '%', '<', '>', '.'
};

static const std::unordered_set<std::string> Keywords = {
    "SELECT",
    "FROM",
    "WHERE",
    "GROUP",
    "BY",
    "HAVING",

    "ORDER",
    "LIMIT",
    "OFFSET",

    "JOIN",
    "INNER",
    "LEFT",
    "RIGHT",
    "FULL",
    "CROSS",
    "ON",
    "USING",

    "AS",

    "WITH",

    "UNION",
    "ALL",
    "DISTINCT",

    "CASE",
    "WHEN",
    "THEN",
    "ELSE",
    "END",

    "AND",
    "OR",
    "NOT",

    "IS",
    "NULL",

    "IN",
    "BETWEEN",
    "LIKE",
    "EXISTS",

    "ASC",
    "DESC",

    "CAST",

    "TRUE",
    "FALSE",

    "DATE",
    "INTERVAL",
};

} // namespace

TTokenStream::TTokenStream(std::istream& in)
    : ITokenStream(in)
{
}

void TTokenStream::Read() {
    auto take = [&]() {
        char ch = 0;
        In.get(ch);
        AdvanceLocation(CurrentLocation, ch);
        return ch;
    };

    auto emitOperator = [&](TOperator op, const std::string& rawValue, TLocation location) {
        Tokens.emplace_back(TToken {
            .Value = {.i64 = (int64_t)op},
            .RawValue = rawValue,
            .Type = TToken::Operator,
            .Location = location,
        });
    };

    auto readQuoted = [&](char quote) {
        std::string value;
        std::string rawValue;
        bool escaped = false;

        while (In.peek() != -1) {
            char ch = take();
            rawValue += ch;
            if (escaped) {
                value += Unescape(ch);
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == quote) {
                rawValue.pop_back();
                return std::make_pair(value, rawValue);
            } else {
                value += ch;
            }
        }

        throw std::runtime_error("unterminated literal");
    };

    auto readNumber = [&](TLocation location) {
        std::string rawValue;
        bool isFloat = false;

        if (In.peek() == '-') {
            rawValue += take();
        }

        if (In.peek() == '.') {
            isFloat = true;
            rawValue += take();
        }

        while (In.peek() != -1 && std::isdigit(In.peek())) {
            rawValue += take();
        }

        if (In.peek() == '.') {
            isFloat = true;
            rawValue += take();
            while (In.peek() != -1 && std::isdigit(In.peek())) {
                rawValue += take();
            }
        }

        if (In.peek() == 'e' || In.peek() == 'E') {
            isFloat = true;
            rawValue += take();
            if (In.peek() == '+' || In.peek() == '-') {
                rawValue += take();
            }
            if (In.peek() == -1 || !std::isdigit(In.peek())) {
                throw std::runtime_error("expected digit in exponent at " + CurrentLocation.ToString());
            }
            while (In.peek() != -1 && std::isdigit(In.peek())) {
                rawValue += take();
            }
        }

        if (isFloat) {
            Tokens.emplace_back(TToken {
                .Value = {.f64 = std::stod(rawValue)},
                .RawValue = rawValue,
                .Type = TToken::Float,
                .Location = location,
            });
        } else {
            Tokens.emplace_back(TToken {
                .Value = {.i64 = std::stoll(rawValue)},
                .RawValue = rawValue,
                .Type = TToken::Integer,
                .Location = location,
            });
        }
    };

    auto isTwoCharOperator = [&](int next) -> bool {
        if (!TwoCharOperatorFirstChars.count(next)) {
            return false;
        }
        In.get();
        auto second = In.peek();
        In.unget();
        std::string opStr;
        opStr += (char)next;
        opStr += (char)second;
        return TwoCharOperators.count(opStr) > 0;
    };

    auto isNumberStart = [&](int next) -> bool {
        if (isdigit(next)) {
            return true;
        }
        if (next == '.') {
            In.get();
            auto second = In.peek();
            In.unget();
            return std::isdigit(second);
        }
        return false;
    };

    while (Tokens.empty() && In.peek() != -1) {
        auto next = In.peek();
        if (std::isspace(next)) {
            take();
            continue;
        }

        TLocation tokenLocation = CurrentLocation;

        if (isTwoCharOperator(next)) {
            std::string name;
            name += take();
            name += take();
            emitOperator(TOperator(name), name, tokenLocation);
        } else if (SingleCharOperators.count(next)) {
            auto ch = take();
            emitOperator(TOperator((uint64_t)ch), std::string(1, ch), tokenLocation);
        } else if (isNumberStart(next)) {
            readNumber(tokenLocation);
        } else if (next == '"' || next == '\'') {
            take();
            auto [value, rawValue] = readQuoted(next);
            Tokens.emplace_back(TToken {
                .Name = value,
                .RawValue = rawValue,
                .Type = TToken::String,
                .Location = tokenLocation,
            });
        } else if (next == '`') {
            take();
            auto [value, rawValue] = readQuoted('`');
            Tokens.emplace_back(TToken {
                .Name = value,
                .RawValue = rawValue,
                .Type = TToken::Identifier,
                .Location = tokenLocation,
            });
        } else {
            std::string name;
            while (In.peek() != -1
                && !std::isspace(In.peek())
                && !SingleCharOperators.count(In.peek())
                && !TwoCharOperatorFirstChars.count(In.peek()))
            {
                name += take();
            }

            std::string upperName;
            for (char c : name) {
                upperName += std::toupper(c);
            }
            if (Keywords.count(upperName)) {
                Tokens.emplace_back(TToken {
                    .Name = upperName,
                    .RawValue = name,
                    .Type = TToken::Keyword,
                    .Location = tokenLocation,
                });
            } else {
                Tokens.emplace_back(TToken {
                    .Name = name,
                    .RawValue = name,
                    .Type = TToken::Identifier,
                    .Location = tokenLocation,
                });
            }
        }
    }
}

} // namespace NSql
} // namespace NQqb