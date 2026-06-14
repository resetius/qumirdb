#include <qdb/sexp/parser.h>

#include <qdb/ops/aggregate.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/join.h>
#include <qdb/ops/project.h>
#include <qdb/ops/source.h>

namespace NQqb {
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
            co_await h.Take(')');
            if (!opts.SourceFactory) {
                co_return TError(loc, "(rel source) requires a SourceFactory");
            }
            co_return opts.SourceFactory(std::string_view(pathTok.Name), loc);
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
            if (keys.empty()) {
                co_return TError(loc, "(rel join) requires at least one (left right) key pair");
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

            // Optional residual predicate, then the closing ')'.
            TExprPtr filter;
            auto tok = h.Next();
            if (!IParseHandle::IsOp(tok, ')')) {
                h.Unget(tok);
                filter = co_await h.Expr();
                co_await h.Take(')');
            }

            auto output = ComputeJoinOutputType(
                left->OutputColumns(), right->OutputColumns(), type);
            if (!output) {
                co_return output.error();
            }
            co_return std::make_shared<TJoinOperator>(
                std::move(left), std::move(right), std::move(keys), type, std::move(filter));
        }

        co_return IParseHandle::MakeError(nameTok, "unknown rel operator: " + nameTok.Name);
    }}};
}

} // namespace NSexp
} // namespace NQqb
