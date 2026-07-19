#include <gtest/gtest.h>

#include <qdb/kernel/annotate_type.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/error.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace NQdb::NKernel;
using namespace NQumir::NAst;

namespace {

TExprPtr Parse(const std::string& src) {
    std::istringstream in(src);
    NCore::TTokenStream tokens(in);
    NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    return *parsed;
}

TStructType Schema() {
    return TStructType({
        {"q", std::make_shared<TFloatType>()},
        {"p", std::make_shared<TFloatType>()},
        {"d", std::make_shared<TFloatType>()},
        {"k", std::make_shared<TIntegerType>(TIntegerType::I32)},
        {"s", std::make_shared<TStringType>()},
    });
}

TStructType NullableSchema() {
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto boolean = std::make_shared<TBoolType>();
    return TStructType({
        {"a", std::make_shared<NQdb::TNullable>(i32)},
        {"b", std::make_shared<NQdb::TNullable>(i32)},
        {"flag", std::make_shared<NQdb::TNullable>(boolean)},
    });
}

bool IsF64(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TFloatType>(t)); }
bool IsInt(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TIntegerType>(t)); }
bool IsBool(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TBoolType>(t)); }
bool IsNullable(const TTypePtr& t) { return NQdb::IsNullableType(t); }

TTypePtr NullableInner(const TTypePtr& t) {
    auto nullable = TMaybeType<NQdb::TNullable>(t).Cast();
    return nullable ? nullable->UnderlyingType : nullptr;
}

bool IsI32(const TTypePtr& t) {
    auto integer = TMaybeType<TIntegerType>(t).Cast();
    return integer && integer->Kind == TIntegerType::I32;
}

} // namespace

TEST(ProjectType, ColumnRef) {
    auto sc = Schema();
    EXPECT_TRUE(IsF64(AnnotateExprType(Parse("p"), sc)));
    EXPECT_TRUE(IsInt(AnnotateExprType(Parse("k"), sc)));
}

TEST(ProjectType, ArithmeticPromotesToF64) {
    auto sc = Schema();
    // p * (1 - d) -> f64 (Q1's disc_price shape).
    EXPECT_TRUE(IsF64(AnnotateExprType(Parse("(* p (- 1 d))"), sc)));
    // p * (1 - d) * (1 + q) -> f64 (charge shape).
    EXPECT_TRUE(IsF64(AnnotateExprType(Parse("(* (* p (- 1 d)) (+ 1 q))"), sc)));
}

TEST(ProjectType, PureIntArithmeticIsI64) {
    EXPECT_TRUE(IsInt(AnnotateExprType(Parse("(+ 1 2)"), Schema())));
    EXPECT_TRUE(IsInt(AnnotateExprType(Parse("(* k 3)"), Schema())));
}

TEST(ProjectType, DivisionForAvgIsF64) {
    // avg = sum(f64) / count(i64) -> f64.
    EXPECT_TRUE(IsF64(AnnotateExprType(Parse("(/ p k)"), Schema())));
}

TEST(ProjectType, ComparisonIsBool) {
    EXPECT_TRUE(IsBool(AnnotateExprType(Parse("(< p 5.0)"), Schema())));
}

TEST(ProjectType, CastTargetType) {
    EXPECT_TRUE(IsF64(AnnotateExprType(Parse("(cast k f64)"), Schema())));
}

TEST(ProjectType, NullableAdditionAnnotates) {
    auto type = AnnotateExprType(Parse("(+ a b)"), NullableSchema());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsI32(NullableInner(type)));
}

TEST(ProjectType, NullableAdditionWithLiteralAnnotates) {
    auto type = AnnotateExprType(Parse("(+ a 1)"), NullableSchema());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsI32(NullableInner(type)));
}

TEST(ProjectType, NullableDivisionAnnotates) {
    auto type = AnnotateExprType(Parse("(/ a b)"), NullableSchema());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsF64(NullableInner(type)));
}

TEST(ProjectType, NullableBoolMixedOverloadAnnotates) {
    auto type = AnnotateExprType(Parse("(&& flag #t)"), NullableSchema());
    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsBool(NullableInner(type)));
}

TEST(ProjectType, NullableIsNullAnnotates) {
    EXPECT_TRUE(IsBool(AnnotateExprType(Parse("(call qdb_is_null a)"), NullableSchema())));
}

TEST(ProjectType, NormalizeNullBranchesWrapsPlainSiblingOfNullableBranch) {
    TStructType sc({
        {"p", std::make_shared<TFloatType>()},
        {"maybe_p", std::make_shared<NQdb::TNullable>(
            std::make_shared<TFloatType>())},
    });

    auto expr = NormalizeNullBranches(
        Parse("(if (< p (: 10.0 f64)) p maybe_p)"),
        sc);
    auto type = AnnotateExprType(expr, sc);

    ASSERT_TRUE(IsNullable(type));
    EXPECT_TRUE(IsF64(NullableInner(type)));
}

TEST(ProjectType, RejectsUnknownColumn) {
    EXPECT_THROW(AnnotateExprType(Parse("missing"), Schema()), NQumir::TError);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
