#include <gtest/gtest.h>

#include <qdb/io/parquet/row_group_predicate.h>

#include <bit>
#include <cstdint>
#include <memory>
#include <limits>
#include <string>
#include <vector>

using namespace NQdb;
using namespace NQumir::NAst;

namespace {

TExprPtr Id(const std::string& name) {
    auto out = std::make_shared<TIdentExpr>(NQumir::TLocation{}, name);
    out->Type = std::make_shared<TIntegerType>(TIntegerType::I64);
    return out;
}

TExprPtr Num(int64_t value) {
    return std::make_shared<TNumberExpr>(NQumir::TLocation{}, value);
}

TExprPtr Bin(std::string op, TExprPtr left, TExprPtr right, TTypePtr type) {
    auto out = std::make_shared<TBinaryExpr>(
        NQumir::TLocation{}, TOperator(std::move(op)),
        std::move(left), std::move(right));
    out->Type = std::move(type);
    return out;
}

TTypePtr I64Type() {
    return std::make_shared<TIntegerType>(TIntegerType::I64);
}

TTypePtr BoolType() {
    return std::make_shared<TBoolType>();
}

TRowGroupColumnRange I64Range(int64_t lo, int64_t hi) {
    return {
        .MinValue = std::bit_cast<uint64_t>(lo),
        .MaxValue = std::bit_cast<uint64_t>(hi),
        .HasBounds = true,
        .MayBeNull = false,
        .MayBeValue = true,
    };
}

TRowGroupColumnRange U64Range(uint64_t lo, uint64_t hi) {
    return {
        .MinValue = lo,
        .MaxValue = hi,
        .HasBounds = true,
        .MayBeNull = false,
        .MayBeValue = true,
    };
}

TRowGroupColumnRange F64Range(double lo, double hi) {
    return {
        .MinValue = std::bit_cast<uint64_t>(lo),
        .MaxValue = std::bit_cast<uint64_t>(hi),
        .HasBounds = true,
        .MayBeNull = false,
        .MayBeValue = true,
        .MayBeNaN = true,
    };
}

struct TTestSchema {
    std::vector<std::string> Names;
    std::vector<TColumnSchema> Columns;
    TSchema Schema;

    explicit TTestSchema(std::vector<std::string> names)
        : Names(std::move(names))
    {
        Columns.reserve(Names.size());
        for (const auto& name : Names) {
            Columns.push_back({name, I64Type()});
        }
        Schema = TSchema{Columns};
    }

    TTestSchema(std::string name, TTypePtr type)
        : Names{std::move(name)}
    {
        Columns.push_back({Names.front(), std::move(type)});
        Schema = TSchema{Columns};
    }
};

} // namespace

TEST(RowGroupPredicate, PrunesSimpleComparison) {
    TTestSchema schema({"x"});
    auto predicate = Bin(">", Id("t.x"), Num(10), BoolType());
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, schema.Schema, "t");
    ASSERT_TRUE(evaluator);

    const std::vector<TRowGroupColumnRange> below{I64Range(0, 10)};
    const std::vector<TRowGroupColumnRange> above{I64Range(11, 20)};
    const std::vector<TRowGroupColumnRange> crossing{I64Range(0, 20)};
    EXPECT_FALSE(evaluator->MayBeTrue(below));
    EXPECT_TRUE(evaluator->MayBeTrue(above));
    EXPECT_TRUE(evaluator->MayBeTrue(crossing));
}

TEST(RowGroupPredicate, EvaluatesMultiColumnArithmeticIntervals) {
    TTestSchema schema({"x", "y"});
    auto sum = Bin("+", Id("x"), Id("y"), I64Type());
    auto predicate = Bin("<", std::move(sum), Num(10), BoolType());
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, schema.Schema, "");
    ASSERT_TRUE(evaluator);

    std::vector<TRowGroupColumnRange> impossible{
        I64Range(10, 20), I64Range(5, 8)};
    std::vector<TRowGroupColumnRange> possible{
        I64Range(0, 20), I64Range(5, 8)};
    EXPECT_FALSE(evaluator->MayBeTrue(impossible));
    EXPECT_TRUE(evaluator->MayBeTrue(possible));
}

TEST(RowGroupPredicate, UnknownConjunctDoesNotHideFalseSupportedConjunct) {
    TTestSchema schema({"x"});
    auto supported = Bin(">", Id("x"), Num(10), BoolType());
    auto unsupported = std::make_shared<TCallExpr>(
        NQumir::TLocation{},
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "unsupported"),
        std::vector<TExprPtr>{Id("x")});
    unsupported->Type = BoolType();
    auto predicate = Bin("&&", supported, unsupported, BoolType());
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, schema.Schema, "");
    ASSERT_TRUE(evaluator);

    const std::vector<TRowGroupColumnRange> below{I64Range(0, 5)};
    EXPECT_FALSE(evaluator->MayBeTrue(below));
}

TEST(RowGroupPredicate, UnknownDisjunctPreventsPruning) {
    TTestSchema schema({"x"});
    auto supported = Bin(">", Id("x"), Num(10), BoolType());
    auto unsupported = std::make_shared<TCallExpr>(
        NQumir::TLocation{},
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "unsupported"),
        std::vector<TExprPtr>{Id("x")});
    unsupported->Type = BoolType();
    auto predicate = Bin("||", supported, unsupported, BoolType());
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, schema.Schema, "");
    ASSERT_TRUE(evaluator);

    const std::vector<TRowGroupColumnRange> below{I64Range(0, 5)};
    EXPECT_TRUE(evaluator->MayBeTrue(below));
}

TEST(RowGroupPredicate, MultiplicationKeepsInteriorPossibilities) {
    TTestSchema schema({"x"});
    auto square = Bin("*", Id("x"), Id("x"), I64Type());
    auto predicate = Bin("<", std::move(square), Num(1), BoolType());
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, schema.Schema, "");
    ASSERT_TRUE(evaluator);

    const std::vector<TRowGroupColumnRange> crossesZero{I64Range(-2, 2)};
    const std::vector<TRowGroupColumnRange> positive{I64Range(2, 3)};
    EXPECT_TRUE(evaluator->MayBeTrue(crossesZero));
    EXPECT_FALSE(evaluator->MayBeTrue(positive));
}

TEST(RowGroupPredicate, OverflowAndMissingBoundsStayConservative) {
    TTestSchema schema({"x"});
    auto sum = Bin("+", Id("x"), Num(1), I64Type());
    auto predicate = Bin("<", std::move(sum), Num(0), BoolType());
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, schema.Schema, "");
    ASSERT_TRUE(evaluator);

    const auto max = std::numeric_limits<int64_t>::max();
    const std::vector<TRowGroupColumnRange> overflow{I64Range(max, max)};
    std::vector<TRowGroupColumnRange> missing{I64Range(10, 20)};
    missing[0].HasBounds = false;
    EXPECT_TRUE(evaluator->MayBeTrue(overflow));
    EXPECT_TRUE(evaluator->MayBeTrue(missing));
}

TEST(RowGroupPredicate, NullOnlyRangeCannotPassWhere) {
    TTestSchema schema({"x"});
    auto predicate = Bin(">", Id("x"), Num(10), BoolType());
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, schema.Schema, "");
    ASSERT_TRUE(evaluator);

    auto nullOnly = I64Range(0, 0);
    nullOnly.HasBounds = false;
    nullOnly.MayBeNull = true;
    nullOnly.MayBeValue = false;
    const std::vector<TRowGroupColumnRange> columns{nullOnly};
    EXPECT_FALSE(evaluator->MayBeTrue(columns));
}

TEST(RowGroupPredicate, SupportsUnsignedRangesAndOverflowFallback) {
    TTestSchema schema(
        "x", std::make_shared<TIntegerType>(TIntegerType::U64));
    auto predicate = Bin(">", Id("x"), Num(10), BoolType());
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, schema.Schema, "");
    ASSERT_TRUE(evaluator);

    const std::vector<TRowGroupColumnRange> below{U64Range(0, 10)};
    const std::vector<TRowGroupColumnRange> above{U64Range(11, 20)};
    EXPECT_FALSE(evaluator->MayBeTrue(below));
    EXPECT_TRUE(evaluator->MayBeTrue(above));

    auto sum = Bin("+", Id("x"), Num(1),
        std::make_shared<TIntegerType>(TIntegerType::U64));
    auto wrappedPredicate = Bin("<", std::move(sum), Num(1), BoolType());
    auto wrappedEvaluator = TRowGroupPredicateEvaluator::Compile(
        wrappedPredicate, schema.Schema, "");
    ASSERT_TRUE(wrappedEvaluator);
    const auto max = std::numeric_limits<uint64_t>::max();
    const std::vector<TRowGroupColumnRange> overflow{U64Range(max, max)};
    EXPECT_TRUE(wrappedEvaluator->MayBeTrue(overflow));
}

TEST(RowGroupPredicate, SupportsFloatArithmeticWithOutwardBounds) {
    TTestSchema schema("x", std::make_shared<TFloatType>());
    auto offset = std::make_shared<TNumberExpr>(NQumir::TLocation{}, 0.1);
    auto sum = Bin("+", Id("x"), offset, std::make_shared<TFloatType>());
    auto zero = std::make_shared<TNumberExpr>(NQumir::TLocation{}, 0.0);
    auto predicate = Bin("<", std::move(sum), zero, BoolType());
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, schema.Schema, "");
    ASSERT_TRUE(evaluator);

    const std::vector<TRowGroupColumnRange> positive{F64Range(1.0, 2.0)};
    const std::vector<TRowGroupColumnRange> negative{F64Range(-1.0, -0.5)};
    EXPECT_FALSE(evaluator->MayBeTrue(positive));
    EXPECT_TRUE(evaluator->MayBeTrue(negative));
}

TEST(RowGroupPredicate, SupportsNotAndNonStrictComparison) {
    TTestSchema schema({"x"});
    auto atMostTen = Bin("<=", Id("x"), Num(10), BoolType());
    auto predicate = std::make_shared<TUnaryExpr>(
        NQumir::TLocation{}, TOperator("!"), std::move(atMostTen));
    predicate->Type = BoolType();
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, schema.Schema, "");
    ASSERT_TRUE(evaluator);

    const std::vector<TRowGroupColumnRange> below{I64Range(0, 10)};
    const std::vector<TRowGroupColumnRange> above{I64Range(11, 20)};
    EXPECT_FALSE(evaluator->MayBeTrue(below));
    EXPECT_TRUE(evaluator->MayBeTrue(above));
}

TEST(RowGroupPredicate, ComparesArithmeticExpressions) {
    TTestSchema schema({"a", "b", "c"});
    auto product = Bin("*", Id("a"), Id("b"), I64Type());
    auto predicate = Bin(">=", std::move(product), Id("c"), BoolType());
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, schema.Schema, "");
    ASSERT_TRUE(evaluator);

    const std::vector<TRowGroupColumnRange> impossible{
        I64Range(2, 3), I64Range(4, 5), I64Range(20, 30)};
    const std::vector<TRowGroupColumnRange> possible{
        I64Range(2, 3), I64Range(4, 5), I64Range(10, 20)};
    EXPECT_FALSE(evaluator->MayBeTrue(impossible));
    EXPECT_TRUE(evaluator->MayBeTrue(possible));
}

TEST(RowGroupPredicate, NeverPrunesAnExhaustiveIntegerWitness) {
    TTestSchema schema({"x", "y"});
    auto product = Bin("*", Id("x"), Id("y"), I64Type());
    auto expression = Bin("+", std::move(product), Id("x"), I64Type());
    auto predicate = Bin("<", std::move(expression), Num(5), BoolType());
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, schema.Schema, "");
    ASSERT_TRUE(evaluator);

    for (int64_t xLo = -3; xLo <= 3; ++xLo) {
        for (int64_t xHi = xLo; xHi <= 3; ++xHi) {
            for (int64_t yLo = -3; yLo <= 3; ++yLo) {
                for (int64_t yHi = yLo; yHi <= 3; ++yHi) {
                    const std::vector<TRowGroupColumnRange> ranges{
                        I64Range(xLo, xHi), I64Range(yLo, yHi)};
                    if (evaluator->MayBeTrue(ranges)) {
                        continue;
                    }
                    for (int64_t x = xLo; x <= xHi; ++x) {
                        for (int64_t y = yLo; y <= yHi; ++y) {
                            EXPECT_FALSE(x * y + x < 5)
                                << "x range [" << xLo << ", " << xHi
                                << "], y range [" << yLo << ", " << yHi
                                << "], witness (" << x << ", " << y << ')';
                        }
                    }
                }
            }
        }
    }
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
