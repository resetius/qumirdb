#include "parser.h"

#include <qumir/optional.h>

/*

<query> ::= <select_stmt> [ ";" ]

<select_stmt> ::=
    [ <with_clause> ]
    <select_core>
    [ <order_by_clause> ]
    [ <limit_clause> ]
    [ <offset_clause> ]

<with_clause> ::=
    "WITH" [ "RECURSIVE" ] <cte_def> { "," <cte_def> }

<cte_def> ::=
    <ident> [ "(" <ident_list> ")" ] "AS" "(" <select_stmt> ")"

<select_core> ::=
    "SELECT" [ <set_quantifier> ] <select_list>
    [ <from_clause> ]
    [ <where_clause> ]
    [ <group_by_clause> ]
    [ <having_clause> ]

<set_quantifier> ::= "ALL" | "DISTINCT"

<select_list> ::=
      "*"
    | <select_item> { "," <select_item> }

<select_item> ::=
      <expr> [ [ "AS" ] <ident> ]
    | <qualified_name> "." "*"

<from_clause> ::=
    "FROM" <table_ref> { "," <table_ref> }

<table_ref> ::=
    <table_factor> { <join_clause> }

<table_factor> ::=
      <qualified_name> [ [ "AS" ] <ident> ]
    | "(" <select_stmt> ")" [ [ "AS" ] <ident> ]
    | "(" <table_ref> ")"

<join_clause> ::=
    [ <join_type> ] "JOIN" <table_factor> <join_condition>

<join_type> ::=
      "INNER"
    | "LEFT" [ "OUTER" ]
    | "RIGHT" [ "OUTER" ]
    | "FULL" [ "OUTER" ]
    | "CROSS"

<join_condition> ::=
      "ON" <expr>
    | "USING" "(" <ident_list> ")"
    | ε

<where_clause> ::= "WHERE" <expr>

<group_by_clause> ::=
    "GROUP" "BY" <group_item> { "," <group_item> }

<group_item> ::= <expr>

<having_clause> ::= "HAVING" <expr>

<order_by_clause> ::=
    "ORDER" "BY" <order_item> { "," <order_item> }

<order_item> ::=
    <expr> [ "ASC" | "DESC" ] [ "NULLS" ( "FIRST" | "LAST" ) ]

<limit_clause> ::= "LIMIT" <expr>

<offset_clause> ::= "OFFSET" <expr>

// exprs:
<expr> ::= <or_expr>

<or_expr> ::=
    <and_expr> { "OR" <and_expr> }

<and_expr> ::=
    <not_expr> { "AND" <not_expr> }

<not_expr> ::=
      "NOT" <not_expr>
    | <comparison_expr>

<comparison_expr> ::=
    <add_expr>
    [
        <comparison_op> <add_expr>
      | "IS" [ "NOT" ] "NULL"
      | [ "NOT" ] "IN" "(" <expr_list_or_subquery> ")"
      | [ "NOT" ] "BETWEEN" <add_expr> "AND" <add_expr>
      | [ "NOT" ] "LIKE" <add_expr>
    ]

<comparison_op> ::=
      "=" | "<>" | "!="
    | "<" | "<=" | ">" | ">="

<add_expr> ::=
    <mul_expr> { ( "+" | "-" ) <mul_expr> }

<mul_expr> ::=
    <unary_expr> { ( "*" | "/" | "%" ) <unary_expr> }

<unary_expr> ::=
      ( "+" | "-" ) <unary_expr>
    | <postfix_expr>

<postfix_expr> ::=
    <primary_expr>
    {
        "::" <type_name>
    }

<primary_expr> ::=
      <literal>
    | <qualified_name>
    | <function_call>
    | <case_expr>
    | "CAST" "(" <expr> "AS" <type_name> ")"
    | "EXISTS" "(" <select_stmt> ")"
    | "(" <expr> ")"
    | "(" <select_stmt> ")"

<function_call> ::=
    <qualified_name> "(" [ <function_args> ] ")"

<function_args> ::=
      "*"
    | <expr> { "," <expr> }

<case_expr> ::=
    "CASE"
        { "WHEN" <expr> "THEN" <expr> }
        [ "ELSE" <expr> ]
    "END"

<expr_list_or_subquery> ::=
      <select_stmt>
    | <expr> { "," <expr> }

<expr_list> ::= <expr> { "," <expr> }

// literals:
<qualified_name> ::= <ident> { "." <ident> }

<ident_list> ::= <ident> { "," <ident> }

<ident> ::= <regular_ident> | <quoted_ident>

<regular_ident> ::= /[A-Za-z_][A-Za-z0-9_]*/

/*
<quoted_ident> ::= '"' { any_char_except_quote | '""' } '"'

<literal> ::=
      <number_literal>
    | <string_literal>
    | "NULL"
    | "TRUE"
    | "FALSE"

<number_literal> ::= /[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?/

<string_literal> ::= "'" { any_char_except_quote | "''" } "'"

<type_name> ::=
      "BOOL"
    | "BOOLEAN"
    | "INT"
    | "INTEGER"
    | "BIGINT"
    | "FLOAT"
    | "DOUBLE"
    | "TEXT"
    | "VARCHAR"
    | "DATE"
    | "TIMESTAMP"
    | "DECIMAL" [ "(" <integer> [ "," <integer> ] ")" ]

<integer> ::= /[0-9]+/

*/

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
TAstTask<TSqlSelectList> select_list(TParserContext& ctx);
TAstTask<TSqlOrder> order_by_clause(TParserContext& ctx);
TAstTask<TSqlWithClause> with_clause(TParserContext& ctx);
TAstTask<TSqlCte> cte_def(TParserContext& ctx);
TAstTask<TSqlTableRef> from_clause(TParserContext& ctx);
TAstTask<TSqlGroupBy> group_by_clause(TParserContext& ctx);
TAstExprTask limit_clause(TParserContext& ctx);
TAstExprTask offset_clause(TParserContext& ctx);
TAstExprTask having_clause(TParserContext& ctx);

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
    auto select = std::make_shared<TSqlSelect>();
    auto token = ctx.Stream.Next();
    if (!IsKeyword(token, "SELECT")) {
        co_return Error(token, "`SELECT' required");
    }
    token = ctx.Stream.Next();
    if (IsKeyword(token, "DISTINCT")) {
        select->Quantifier = ESetQuantifier::Distinct;
    } else if (IsKeyword(token, "ALL")) {
        select->Quantifier = ESetQuantifier::All;
    } else {
        ctx.Stream.Unget(token);
    }
    select->SelectList = co_await select_list(ctx);

    select->From = co_await TryKeywords(from_clause, ctx, {"FROM"});
    select->GroupBy = co_await TryKeywords(group_by_clause, ctx, {"GROUP BY"});
    select->Having = co_await TryKeywords(having_clause, ctx, {"HAVING"});

    co_return nullptr;
}

TAstTask<TSqlSelectList> select_list(TParserContext& ctx) {
    co_return nullptr;
}

TAstTask<TSqlTableRef> from_clause(TParserContext& ctx) {
    co_return nullptr;
}

TAstTask<TSqlGroupBy> group_by_clause(TParserContext& ctx) {
    co_return nullptr;
}

TAstExprTask having_clause(TParserContext& ctx) {
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
