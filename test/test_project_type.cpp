#include <gtest/gtest.h>

#include <qdb/kernel/project_type.h>

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

bool IsF64(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TFloatType>(t)); }
bool IsInt(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TIntegerType>(t)); }
bool IsBool(const TTypePtr& t) { return static_cast<bool>(TMaybeType<TBoolType>(t)); }

} // namespace

TEST(ProjectType, ColumnRef) {
    auto sc = Schema();
    EXPECT_TRUE(IsF64(InferProjectExprType(Parse("p"), sc)));
    EXPECT_TRUE(IsInt(InferProjectExprType(Parse("k"), sc)));
}

TEST(ProjectType, ArithmeticPromotesToF64) {
    auto sc = Schema();
    // p * (1 - d) -> f64 (Q1's disc_price shape).
    EXPECT_TRUE(IsF64(InferProjectExprType(Parse("(* p (- 1 d))"), sc)));
    // p * (1 - d) * (1 + q) -> f64 (charge shape).
    EXPECT_TRUE(IsF64(InferProjectExprType(Parse("(* (* p (- 1 d)) (+ 1 q))"), sc)));
}

TEST(ProjectType, PureIntArithmeticIsI64) {
    EXPECT_TRUE(IsInt(InferProjectExprType(Parse("(+ 1 2)"), Schema())));
    EXPECT_TRUE(IsInt(InferProjectExprType(Parse("(* k 3)"), Schema())));
}

TEST(ProjectType, DivisionForAvgIsF64) {
    // avg = sum(f64) / count(i64) -> f64.
    EXPECT_TRUE(IsF64(InferProjectExprType(Parse("(/ p k)"), Schema())));
}

TEST(ProjectType, ComparisonIsBool) {
    EXPECT_TRUE(IsBool(InferProjectExprType(Parse("(< p 5.0)"), Schema())));
}

TEST(ProjectType, CastTargetType) {
    EXPECT_TRUE(IsF64(InferProjectExprType(Parse("(: k f64)"), Schema())));
}

TEST(ProjectType, RejectsUnknownColumn) {
    EXPECT_THROW(InferProjectExprType(Parse("missing"), Schema()), NQumir::TError);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
