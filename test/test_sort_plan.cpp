#include <gtest/gtest.h>

#include <qdb/io/schema.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/sql/parser.h>

#include <qumir/parser/type.h>

#include <expected>
#include <sstream>
#include <string_view>
#include <vector>

using namespace NQdb;

namespace {

struct TStubSource : ISource {
    explicit TStubSource(std::vector<TColumnSchema> columns)
        : Cols_(std::move(columns))
        , Schema_{Cols_}
    {}

    const TSchema& Schema() const override { return Schema_; }
    bool Next(TRowSet&) override { return false; }

    std::vector<TColumnSchema> Cols_;
    TSchema Schema_;
};

std::expected<TOperatorPtr, NQumir::TError> BuildSqlPlan(std::string_view sql, ISource& source) {
    std::istringstream in{std::string(sql)};
    NSql::TTokenStream ts(in);
    NSql::TParser parser;
    auto parsed = parser.Parse(ts);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    auto factory = [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::make_shared<TSourceOperator>(source, std::string(table));
    };

    return BuildPlan(parsed.value(), factory);
}

} // namespace

TEST(SortPlan, BuildPlanWrapsOrderByAfterProjection) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TStubSource source({{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan("SELECT a AS k FROM t ORDER BY k DESC NULLS LAST", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto sort = TMaybeOp<TSortOperator>(*plan);
    ASSERT_TRUE(sort);
    ASSERT_EQ(sort.Cast()->Keys().size(), 1u);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "k");
    EXPECT_EQ(sort.Cast()->Keys()[0].Direction, ESortDirection::Desc);
    EXPECT_EQ(sort.Cast()->Keys()[0].Nulls, ESortNulls::Last);

    auto project = TMaybeOp<TProjectOperator>(sort.Cast()->Input());
    ASSERT_TRUE(project);
    ASSERT_EQ(project.Cast()->Projections().size(), 1u);
    EXPECT_EQ(project.Cast()->Projections()[0].Name, "k");
}

TEST(SortPlan, BuildPlanRejectsOrderByExpressionForMvp) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TStubSource source({{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan("SELECT a FROM t ORDER BY a + 1", source);
    ASSERT_FALSE(plan.has_value());
    EXPECT_NE(plan.error().ToString().find("ORDER BY currently supports only output column identifiers"),
        std::string::npos);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
