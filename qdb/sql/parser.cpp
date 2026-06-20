#include "parser.h"

#include <qumir/optional.h>

namespace NQdb {
namespace NSql {

using NQumir::TError;
using NQumir::TLocation;
using NQumir::NAst::TToken;
using NQumir::NAst::TWrappedTokenStream;

namespace {

template<class T>
using TAstTask = NQumir::TExpectedTask<TSqlPtr<T>, TError, TLocation>;

using TAstExprTask = NQumir::TExpectedTask<NQumir::NAst::TExprPtr, TError, TLocation>;
using TVoidTask = NQumir::TExpectedTask<std::monostate, TError, TLocation>;

struct TParserContext {
    TWrappedTokenStream& Stream;

    TParserContext(TWrappedTokenStream& stream)
        : Stream(stream)
    { }
};

TError Error(const TToken& token, const std::string& message) {
    return TError(token.Location, message);
}

bool IsEof(const TToken& token) {
    return token.Type == TToken::Operator && token.Value.i64 == -1;
}

bool IsOp(const TToken& token, char op) {
    return token.Type == TToken::Operator && token.Value.i64 == static_cast<int64_t>(op);
}

bool IsKeyword(const TToken& tok, const std::string& kw) {
    return tok.Type == TToken::Keyword && tok.Name == kw;
}

template<class F>
using TTask = std::invoke_result_t<F&, TParserContext&>;

template<typename T>
TTask<T> TryKeywords(T&& lambda, TParserContext& ctx, const std::vector<std::string>& kws) {
    for (const auto& kw : kws) {
        auto token = ctx.Stream.Next();
        if (!IsKeyword(token, kw)) {
            co_return {};
        }
    }

    co_return co_await lambda(ctx);
}

TAstTask<TSqlQuery> select_stmt(TParserContext& ctx);
TAstTask<TSqlNode> select_core(TParserContext& ctx);
TAstTask<TSqlOrder> order_by_clause(TParserContext& ctx);
TAstTask<TSqlWithClause> with_clause(TParserContext& ctx);
TAstTask<TSqlCte> cte_def(TParserContext& ctx);
TAstExprTask limit_clause(TParserContext& ctx);
TAstExprTask offset_clause(TParserContext& ctx);

TAstTask<TSqlQuery> query(TParserContext& ctx) {
    TSqlPtr<TSqlQuery> q;

    q = co_await select_stmt(ctx);

    auto token = ctx.Stream.Next();
    if (IsEof(token) || IsOp(token, ';')) {
        co_return q;
    }

    co_return Error(token, "expected ';' or end of file");
}

TAstTask<TSqlQuery> select_stmt(TParserContext& ctx) {
    TSqlPtr<TSqlQuery> q = std::make_shared<TSqlQuery>();
    auto token = ctx.Stream.Next();

    q->WithClause = co_await TryKeywords(with_clause, ctx, {"WITH"});

    q->Body = co_await select_core(ctx);

    q->OrderBy = co_await TryKeywords(order_by_clause, ctx, {"ORDER", "BY"});
    q->Limit = co_await TryKeywords(limit_clause, ctx, {"LIMIT"});
    q->Offset = co_await TryKeywords(offset_clause, ctx, {"OFFSET"});

    co_return q;
}

TAstTask<TSqlWithClause> with_clause(TParserContext& ctx) {
    auto with = std::make_shared<TSqlWithClause>();
    auto token = ctx.Stream.Next();
    if (IsKeyword(token, "RECURSIVE")) {
        with->Recursive = true;
    } else {
        ctx.Stream.Unget(token);
    }

    do {
        auto cte = co_await cte_def(ctx);
        token = ctx.Stream.Next();
    } while (IsOp(token, ','));
    ctx.Stream.Unget(token);

    co_return with;
}

TAstTask<TSqlNode> select_core(TParserContext& ctx) {
    co_return nullptr;
}

TAstTask<TSqlOrder> order_by_clause(TParserContext& ctx) {
    co_return nullptr;
}

TAstExprTask limit_clause(TParserContext& ctx) {
    co_return nullptr;
}

TAstExprTask offset_clause(TParserContext& ctx) {
    co_return nullptr;
}

TAstTask<TSqlCte> cte_def(TParserContext& ctx) {
    co_return nullptr;
}

} // namespace

std::expected<TSqlNodePtr, NQumir::TError> Parse(TTokenStream& stream)
{
    TWrappedTokenStream wrappedStream(stream, /*windowSize*/ 10);
    TParserContext context(wrappedStream);
    return nullptr;
}

} // namespace NSql
} // namespace NQdb
