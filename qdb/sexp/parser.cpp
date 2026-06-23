#include <qdb/sexp/parser.h>

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>

namespace NQdb {
namespace NSexp {

using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;
using NQumir::TError;
using NQumir::TLocation;

TNodeParserMap MakeRelParsers(TRelParserOptions options) {
    return {{"rel", [opts = std::move(options)](IParseHandle& h, TLocation loc) -> TAstTask {
        auto nameTok = h.Next();
        if (nameTok.Type != TToken::Identifier) {
            co_return IParseHandle::MakeError(nameTok, "expected rel operator name after 'rel'");
        }

        if (nameTok.Name == TSourceOperator::OpId) {
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

        if (nameTok.Name == TFilterOperator::OpId) {
            auto inputExpr = co_await h.Expr();
            auto input = std::static_pointer_cast<IOperator>(inputExpr);
            auto pred = co_await h.Expr();
            co_await h.Take(')');
            co_return std::make_shared<TFilterOperator>(std::move(input), std::move(pred));
        }

        if (nameTok.Name == TProjectOperator::OpId) {
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

        if (nameTok.Name == TAggregateOperator::OpId) {
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

        if (nameTok.Name == TJoinOperator::OpId) {
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

        co_return IParseHandle::MakeError(nameTok, "unknown rel operator: " + nameTok.Name);
    }}};
}

} // namespace NSexp
} // namespace NQdb
