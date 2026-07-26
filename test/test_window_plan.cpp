#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/plan/build.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/window.h>
#include <qdb/plan/pipeline.h>
#include <qdb/sql/parser.h>

#include <qumir/parser/ast.h>

#include <expected>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

using namespace NQdb;

namespace {

std::expected<TOperatorPtr, NQumir::TError> BuildSqlPlan(
    std::string_view sql, const std::map<std::string, ISource*>& tables)
{
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
        auto it = tables.find(std::string(table));
        if (it == tables.end()) {
            return std::unexpected(NQumir::TError("unknown table: " + std::string(table)));
        }
        return std::make_shared<TSourceOperator>(*it->second, std::string(table));
    };
    return BuildPlan(parsed.value(), factory);
}

const TWindowOperator* FindWindow(const TOperatorPtr& op) {
    if (auto* window = dynamic_cast<const TWindowOperator*>(op.get())) {
        return window;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (auto* found = FindWindow(childOp.Cast())) {
                return found;
            }
        }
    }
    return nullptr;
}

int CountWindows(const TOperatorPtr& op) {
    int count = dynamic_cast<const TWindowOperator*>(op.get()) ? 1 : 0;
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            count += CountWindows(childOp.Cast());
        }
    }
    return count;
}

const TJoinOperator* FindJoin(const TOperatorPtr& op) {
    if (auto* join = dynamic_cast<const TJoinOperator*>(op.get())) {
        return join;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (auto* found = FindJoin(childOp.Cast())) {
                return found;
            }
        }
    }
    return nullptr;
}

} // namespace

TEST(WindowPlan, SingleSpecStructure) {
    NQdb::TMockSource t({"a", "b", "c"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "SELECT rank() OVER (PARTITION BY a ORDER BY b DESC) AS r FROM t", tables);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto* window = FindWindow(*plan);
    ASSERT_NE(window, nullptr);
    ASSERT_EQ(window->PartitionKeys().size(), 1u);
    EXPECT_EQ(window->PartitionKeys()[0], "a");
    ASSERT_EQ(window->OrderKeys().size(), 1u);
    EXPECT_EQ(window->OrderKeys()[0].Column, "b");
    EXPECT_EQ(window->OrderKeys()[0].Direction, ESortDirection::Desc);
    ASSERT_EQ(window->Functions().size(), 1u);
    EXPECT_EQ(window->Functions()[0].Func, "rank");
    EXPECT_EQ(window->Functions()[0].Arg, nullptr);
    // ORDER BY present with no explicit frame -> RANGE ... CURRENT ROW.
    ASSERT_TRUE(window->Frame().has_value());
    EXPECT_EQ(window->Frame()->Mode, EWindowFrameMode::Range);
    EXPECT_EQ(window->Frame()->Start.Kind, EFrameBoundKind::UnboundedPreceding);
    EXPECT_EQ(window->Frame()->End.Kind, EFrameBoundKind::CurrentRow);
}

TEST(WindowPlan, DistinctSpecsStack) {
    NQdb::TMockSource t({"a", "b", "x"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    // Two functions share the (partition a) spec -> one node; a different spec
    // (partition b) -> a second, stacked node.
    auto plan = BuildSqlPlan(
        "SELECT rank() OVER (PARTITION BY a) AS r1, "
        "sum(x) OVER (PARTITION BY a) AS s1, "
        "rank() OVER (PARTITION BY b) AS r2 FROM t", tables);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    EXPECT_EQ(CountWindows(*plan), 2);
}

TEST(WindowPlan, FullPipelineOverJoin) {
    NQdb::TMockSource orders({"o_orderkey", "o_custkey", "o_total"});
    NQdb::TMockSource customer({"c_custkey", "c_name"});
    std::map<std::string, ISource*> tables = {{"orders", &orders}, {"customer", &customer}};

    auto plan = BuildSqlPlan(
        "SELECT c_name, "
        "rank() OVER (PARTITION BY c_custkey ORDER BY o_total DESC) AS r "
        "FROM orders, customer WHERE o_custkey = c_custkey", tables);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    // Full pipeline must run through the window (join reorder + equi-key
    // extraction + column pruning all recurse into the window's child).
    ApplyPlanPasses(*plan);

    EXPECT_EQ(CountWindows(*plan), 1);
    // The join equality below the window was lifted into a join key.
    auto* join = FindJoin(*plan);
    ASSERT_NE(join, nullptr);
    EXPECT_FALSE(join->Keys().empty());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
