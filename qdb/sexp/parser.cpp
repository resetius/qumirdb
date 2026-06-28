#include <qdb/sexp/parser.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>

#include <optional>

namespace NQdb {
namespace NSexp {

using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;
using NQumir::TError;
using NQumir::TLocation;

std::optional<std::string> ReadIdentifier(IParseHandle& h, TToken token) {
    if (token.Type != TToken::Identifier) {
        return std::nullopt;
    }
    std::string result = token.Name;
    while (true) {
        auto dash = h.Next();
        const bool isDash = IParseHandle::IsOp(dash, '-')
            || (dash.Type == TToken::Identifier && dash.Name == "-");
        if (!isDash) {
            h.Unget(dash);
            break;
        }
        auto part = h.Next();
        if (part.Type != TToken::Identifier) {
            h.Unget(part);
            h.Unget(dash);
            break;
        }
        result += "-";
        result += part.Name;
    }
    return result;
}

std::optional<ESortDirection> ParseSortDirection(std::string_view name) {
    if (name == "asc") return ESortDirection::Asc;
    if (name == "desc") return ESortDirection::Desc;
    return std::nullopt;
}

std::optional<ESortNulls> ParseSortNulls(std::string_view name) {
    if (name == "nulls-default") return ESortNulls::Default;
    if (name == "nulls-first") return ESortNulls::First;
    if (name == "nulls-last") return ESortNulls::Last;
    return std::nullopt;
}

TNodeParserMap MakeRelParsers(TRelParserOptions options) {
    return {{"rel", [opts = std::move(options)](IParseHandle& h, TLocation loc) -> TAstTask {
        auto nameTok = h.Next();
        auto relName = ReadIdentifier(h, nameTok);
        if (!relName) {
            co_return IParseHandle::MakeError(nameTok, "expected rel operator name after 'rel'");
        }

        if (*relName == TSourceOperator::OpId) {
            auto pathTok = h.Next();
            if (pathTok.Type != TToken::String) {
                co_return IParseHandle::MakeError(pathTok, "(rel source) expects a path string");
            }
            // Optional explicit alias: (rel source "path" "alias")
            std::string explicitAlias;
            auto peek = h.Next();
            if (peek.Type == TToken::String) {
                explicitAlias = peek.Name;
                co_await h.Take(')');
            } else if (!IParseHandle::IsOp(peek, ')')) {
                co_return IParseHandle::MakeError(peek, "(rel source) expects ')' or alias string");
            }
            if (!opts.SourceFactory) {
                co_return TError(loc, "(rel source) requires a SourceFactory");
            }
            auto opExpr = opts.SourceFactory(std::string_view(pathTok.Name), loc);
            if (!opExpr) co_return TError(loc, "(rel source) factory returned null");
            if (!explicitAlias.empty()) {
                if (auto* src = dynamic_cast<TSourceOperator*>(opExpr.get())) {
                    src->SetAlias(std::move(explicitAlias));
                }
            }
            co_return opExpr;
        }

        if (*relName == TFilterOperator::OpId) {
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);
            auto pred = co_await h.Expr();
            co_await h.Take(')');
            co_return std::make_shared<TFilterOperator>(std::move(input), std::move(pred));
        }

        if (*relName == TProjectOperator::OpId) {
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);
            std::vector<TProjectionSpec> specs;
            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break;
                if (!IParseHandle::IsOp(tok, '(')) {
                    co_return IParseHandle::MakeError(tok, "expected '(' before projection spec");
                }
                auto projName = h.Next();
                if (projName.Type != TToken::Identifier) {
                    co_return IParseHandle::MakeError(projName, "expected projection output name");
                }
                auto projExpr = co_await h.Expr();
                co_await h.Take(')');
                specs.push_back({projName.Name, std::move(projExpr)});
            }
            co_return std::make_shared<TProjectOperator>(std::move(input), std::move(specs));
        }

        if (*relName == TSortOperator::OpId || *relName == TTopSortOperator::OpId) {
            const bool isTopSort = *relName == TTopSortOperator::OpId;
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);
            std::vector<TSortKey> keys;
            std::optional<int64_t> limit;

            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break;
                if (!IParseHandle::IsOp(tok, '(')) {
                    co_return IParseHandle::MakeError(tok, "expected '(' before sort key or limit");
                }
                auto keyTok = h.Next();
                auto keyName = ReadIdentifier(h, keyTok);
                if (!keyName) {
                    co_return IParseHandle::MakeError(keyTok, "expected sort key column or 'limit'");
                }
                if (*keyName == "limit") {
                    auto limitTok = h.Next();
                    if (limitTok.Type != TToken::Integer) {
                        co_return IParseHandle::MakeError(limitTok, "expected integer top-sort limit");
                    }
                    limit = limitTok.Value.i64;
                    co_await h.Take(')');
                    continue;
                }

                auto directionTok = h.Next();
                auto directionName = ReadIdentifier(h, directionTok);
                if (!directionName) {
                    co_return IParseHandle::MakeError(directionTok, "expected sort direction");
                }
                auto direction = ParseSortDirection(*directionName);
                if (!direction) {
                    co_return IParseHandle::MakeError(directionTok, "unknown sort direction: " + *directionName);
                }
                auto nullsTok = h.Next();
                auto nullsName = ReadIdentifier(h, nullsTok);
                if (!nullsName) {
                    co_return IParseHandle::MakeError(nullsTok, "expected sort nulls mode");
                }
                auto nulls = ParseSortNulls(*nullsName);
                if (!nulls) {
                    co_return IParseHandle::MakeError(nullsTok, "unknown sort nulls mode: " + *nullsName);
                }
                co_await h.Take(')');
                keys.push_back({*keyName, *direction, *nulls});
            }

            if (isTopSort) {
                if (!limit) {
                    co_return TError(loc, "(rel top-sort) requires (limit N)");
                }
                co_return std::make_shared<TTopSortOperator>(std::move(input), std::move(keys), *limit);
            }
            if (limit) {
                co_return TError(loc, "(rel sort) does not accept (limit N); use (rel top-sort)");
            }
            co_return std::make_shared<TSortOperator>(std::move(input), std::move(keys));
        }

        if (*relName == TLimitOperator::OpId) {
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);
            std::optional<int64_t> limit;
            int64_t offset = 0;

            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break;
                if (!IParseHandle::IsOp(tok, '(')) {
                    co_return IParseHandle::MakeError(tok, "expected '(' before limit option");
                }
                auto optTok = h.Next();
                auto optName = ReadIdentifier(h, optTok);
                if (!optName) {
                    co_return IParseHandle::MakeError(optTok, "expected limit option name");
                }
                auto valueTok = h.Next();
                if (valueTok.Type != TToken::Integer) {
                    co_return IParseHandle::MakeError(valueTok, "expected integer limit option value");
                }
                if (*optName == "limit") {
                    limit = valueTok.Value.i64;
                } else if (*optName == "offset") {
                    offset = valueTok.Value.i64;
                } else {
                    co_return IParseHandle::MakeError(optTok, "unknown limit option: " + *optName);
                }
                co_await h.Take(')');
            }
            if (!limit) {
                co_return TError(loc, "(rel limit) requires (limit N)");
            }
            co_return std::make_shared<TLimitOperator>(std::move(input), *limit, offset);
        }

        if (*relName == TAggregateOperator::OpId) {
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);

            co_await h.Take('(');
            auto keysTok = h.Next();
            if (keysTok.Type != TToken::Identifier || keysTok.Name != "keys") {
                co_return IParseHandle::MakeError(keysTok, "expected 'keys' after '(' in (rel aggregate ...)");
            }
            std::vector<std::string> groupKeys;
            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break;
                if (tok.Type != TToken::Identifier) {
                    co_return IParseHandle::MakeError(tok, "expected key column name");
                }
                groupKeys.push_back(tok.Name);
            }

            std::vector<TAggregateSpec> aggs;
            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break;
                if (!IParseHandle::IsOp(tok, '(')) {
                    co_return IParseHandle::MakeError(tok, "expected '(' before aggregate spec");
                }
                auto aggTok = h.Next();
                if (aggTok.Type != TToken::Identifier || aggTok.Name != "agg") {
                    co_return IParseHandle::MakeError(aggTok, "expected 'agg'");
                }
                auto outName = h.Next();
                if (outName.Type != TToken::Identifier) {
                    co_return IParseHandle::MakeError(outName, "expected aggregate output name");
                }
                auto funcTok = h.Next();
                if (funcTok.Type != TToken::Identifier) {
                    co_return IParseHandle::MakeError(funcTok, "expected aggregate function name");
                }
                TExprPtr arg;
                auto closeTok = h.Next();
                if (!IParseHandle::IsOp(closeTok, ')')) {
                    h.Unget(closeTok);
                    arg = co_await h.Expr();
                    co_await h.Take(')');
                }
                aggs.push_back({outName.Name, funcTok.Name, std::move(arg)});
            }

            co_return std::make_shared<TAggregateOperator>(std::move(input), std::move(groupKeys), std::move(aggs));
        }

        if (*relName == TJoinOperator::OpId) {
            auto leftExpr = co_await h.Expr();
            auto left = std::static_pointer_cast<IOperator>(leftExpr);
            auto rightExpr = co_await h.Expr();
            auto right = std::static_pointer_cast<IOperator>(rightExpr);

            // Key list: ((lk rk) (lk rk) ...)
            co_await h.Take('(');
            std::vector<TJoinKey> keys;
            while (true) {
                auto tok = h.Next();
                if (IParseHandle::IsOp(tok, ')')) break; // end of key list
                if (!IParseHandle::IsOp(tok, '(')) {
                    co_return IParseHandle::MakeError(tok, "expected '(' before join key pair");
                }
                auto leftKey = h.Next();
                if (leftKey.Type != TToken::Identifier) {
                    co_return IParseHandle::MakeError(leftKey, "expected left key column");
                }
                auto rightKey = h.Next();
                if (rightKey.Type != TToken::Identifier) {
                    co_return IParseHandle::MakeError(rightKey, "expected right key column");
                }
                co_await h.Take(')');
                keys.push_back({leftKey.Name, rightKey.Name});
            }
            // Join type as a bare keyword: (inner)
            co_await h.Take('(');
            auto typeTok = h.Next();
            if (typeTok.Type != TToken::Identifier) {
                co_return IParseHandle::MakeError(typeTok, "expected join type name");
            }
            auto parsedType = ParseJoinType(typeTok.Name);
            if (!parsedType) {
                co_return IParseHandle::MakeError(typeTok, "unknown join type: " + typeTok.Name);
            }
            co_await h.Take(')');
            EJoinType type = *parsedType;
            if (keys.empty() && type != EJoinType::Inner) {
                co_return TError(loc, "cross join (empty key list) only supports inner type");
            }

            // Optional residual predicate, then the closing ')'.
            TExprPtr filter;
            auto tok = h.Next();
            if (!IParseHandle::IsOp(tok, ')')) {
                h.Unget(tok);
                filter = co_await h.Expr();
                co_await h.Take(')');
            }

            co_return std::make_shared<TJoinOperator>(
                std::move(left), std::move(right), std::move(keys), type, std::move(filter));
        }

        co_return IParseHandle::MakeError(nameTok, "unknown rel operator: " + *relName);
    }}};
}

} // namespace NSexp
} // namespace NQdb
