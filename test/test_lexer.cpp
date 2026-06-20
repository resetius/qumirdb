#include <gtest/gtest.h>

#include <qdb/sql/lexer.h>

#include <sstream>
#include <vector>
#include <string>

using namespace NQqb::NSql;
using namespace NQumir::NAst;

namespace {

std::vector<TToken> Tokenize(const std::string& sql) {
    std::istringstream in(sql);
    TTokenStream lexer(in);

    std::vector<TToken> tokens;
    while (true) {
        auto token = lexer.Next();
        // std::cerr << "Token: " << token.RawValue << " (type: " << token.Type << ", location: " << token.Location.ToString() << ")\n";
        if (token.Type == TToken::Operator && token.Value.i64 == (uint64_t)-1) {
            break;
        }
        tokens.push_back(token);
    }
    return tokens;
}

std::vector<std::string> RawTokens(const std::string& sql) {
    std::vector<std::string> result;
    for (const auto& token : Tokenize(sql)) {
        result.push_back(token.RawValue);
    }
    return result;
}

} // namespace

TEST(SqlLexer, SimpleSelect) {
    EXPECT_EQ(
        RawTokens("select a, b from table1 where a >= 10;"),
        (std::vector<std::string>{
            "select", "a", ",", "b", "from", "table1", "where", "a", ">=", "10", ";"
        })
    );
}

TEST(SqlLexer, KeywordsAreUppercased) {
    auto tokens = Tokenize("select from where");
    ASSERT_EQ(tokens.size(), 3u);

    EXPECT_EQ(tokens[0].Type, TToken::Keyword);
    EXPECT_EQ(tokens[0].Name, "SELECT");

    EXPECT_EQ(tokens[1].Type, TToken::Keyword);
    EXPECT_EQ(tokens[1].Name, "FROM");

    EXPECT_EQ(tokens[2].Type, TToken::Keyword);
    EXPECT_EQ(tokens[2].Name, "WHERE");
}

TEST(SqlLexer, IdentifiersPreserveCase) {
    auto tokens = Tokenize("Foo bar_123");
    ASSERT_EQ(tokens.size(), 2u);

    EXPECT_EQ(tokens[0].Type, TToken::Identifier);
    EXPECT_EQ(tokens[0].Name, "Foo");

    EXPECT_EQ(tokens[1].Type, TToken::Identifier);
    EXPECT_EQ(tokens[1].Name, "bar_123");
}

TEST(SqlLexer, QualifiedName) {
    EXPECT_EQ(
        RawTokens("schema.table.column"),
        (std::vector<std::string>{"schema", ".", "table", ".", "column"})
    );
}

TEST(SqlLexer, Operators) {
    EXPECT_EQ(
        RawTokens("a <= b >= c <> d != e && f || g :: int"),
        (std::vector<std::string>{
            "a", "<=", "b", ">=", "c", "<>", "d", "!=", "e", "&&", "f", "||", "g", "::", "int"
        })
    );
}

TEST(SqlLexer, MinusIsOperatorNotPartOfNumber) {
    EXPECT_EQ(
        RawTokens("a-1 -2"),
        (std::vector<std::string>{"a", "-", "1", "-", "2"})
    );
}

TEST(SqlLexer, Numbers) {
    auto tokens = Tokenize("123 12.34 .5 1e10 1.2e-3");
    ASSERT_EQ(tokens.size(), 5u);

    EXPECT_EQ(tokens[0].Type, TToken::Integer);
    EXPECT_EQ(tokens[0].Value.i64, 123);

    EXPECT_EQ(tokens[1].Type, TToken::Float);
    EXPECT_DOUBLE_EQ(tokens[1].Value.f64, 12.34);

    EXPECT_EQ(tokens[2].Type, TToken::Float);
    EXPECT_DOUBLE_EQ(tokens[2].Value.f64, .5);

    EXPECT_EQ(tokens[3].Type, TToken::Float);
    EXPECT_DOUBLE_EQ(tokens[3].Value.f64, 1e10);

    EXPECT_EQ(tokens[4].Type, TToken::Float);
    EXPECT_DOUBLE_EQ(tokens[4].Value.f64, 1.2e-3);
}

TEST(SqlLexer, SqlString) {
    auto tokens = Tokenize("'hello' 'it''s ok'");
    ASSERT_EQ(tokens.size(), 2u);

    EXPECT_EQ(tokens[0].Type, TToken::String);
    EXPECT_EQ(tokens[0].Name, "hello");

    EXPECT_EQ(tokens[1].Type, TToken::String);
    EXPECT_EQ(tokens[1].Name, "it's ok");
}

TEST(SqlLexer, QuotedIdentifier) {
    auto tokens = Tokenize("\"Column Name\"");
    ASSERT_EQ(tokens.size(), 1u);

    EXPECT_EQ(tokens[0].Type, TToken::Identifier);
    EXPECT_EQ(tokens[0].Name, "Column Name");
}

TEST(SqlLexer, BacktickIdentifier) {
    auto tokens = Tokenize("`Column Name`");
    ASSERT_EQ(tokens.size(), 1u);

    EXPECT_EQ(tokens[0].Type, TToken::Identifier);
    EXPECT_EQ(tokens[0].Name, "Column Name");
}

TEST(SqlLexer, LineComment) {
    EXPECT_EQ(
        RawTokens("select -- comment\n a"),
        (std::vector<std::string>{"select", "a"})
    );
}

TEST(SqlLexer, BlockComment) {
    EXPECT_EQ(
        RawTokens("select /* comment */ a"),
        (std::vector<std::string>{"select", "a"})
    );
}

TEST(SqlLexer, UnterminatedStringThrows) {
    EXPECT_THROW(Tokenize("'abc"), std::runtime_error);
}

TEST(SqlLexer, UnterminatedBlockCommentThrows) {
    EXPECT_THROW(Tokenize("select /* abc"), std::runtime_error);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

