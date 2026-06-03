#include <qdb/sexp/parser.h>

#include <qdb/ops/filter.h>
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
            co_await h.Take(')');
            if (!opts.SourceFactory) {
                co_return TError(loc, "(rel source) requires a SourceFactory");
            }
            co_return opts.SourceFactory(loc);
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

        co_return IParseHandle::MakeError(nameTok, "unknown rel operator: " + nameTok.Name);
    }}};
}

} // namespace NSexp
} // namespace NQqb
