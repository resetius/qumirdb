#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/plan/build.h>
#include <qdb/plan/ops/cte_ref.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/cte_reuse.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/sql/parser.h>

#include <expected>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

using namespace NQdb;

namespace {

TOperatorPtr BuildSqlPlan(std::string_view sql, const std::map<std::string, ISource*>& tables) {
    std::istringstream in{std::string(sql)};
    NSql::TTokenStream ts(in);
    NSql::TParser parser;
    auto parsed = parser.Parse(ts);
    EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().ToString());
    auto factory = [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        auto it = tables.find(std::string(table));
        if (it == tables.end()) {
            return std::unexpected(NQumir::TError("unknown table: " + std::string(table)));
        }
        return std::make_shared<TSourceOperator>(*it->second, std::string(table));
    };
    auto plan = BuildPlan(parsed.value(), factory);
    EXPECT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());
    return plan.value();
}

void Annotate(const TOperatorPtr& plan) {
    AssignSourceAliases(plan);
    QualifyColumns(plan);
    AnnotateTypes(plan);
}

// Builds and annotates main + every definition, then propagates demand (as the
// pipeline does up to the reuse decision).
TCteUsageMap Analyze(const TOperatorPtr& plan) {
    for (const auto& def : CollectCteDefinitions(plan)) {
        Annotate(def->Plan);
    }
    Annotate(plan);
    return PropagateCteDemands(plan);
}

std::multiset<size_t> RefCounts(const TCteUsageMap& usage) {
    std::multiset<size_t> counts;
    for (const auto& [_, info] : usage) {
        counts.insert(info.StaticRefCount);
    }
    return counts;
}

bool ContainsCteRef(const TOperatorPtr& op) {
    if (!op) {
        return false;
    }
    if (TMaybeOp<TCteRef>(op)) {
        return true;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (ContainsCteRef(childOp.Cast())) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

TEST(CteReuse, RefcountDecidesMode) {
    auto single = std::make_shared<TCteDefinition>();
    auto shared = std::make_shared<TCteDefinition>();

    TCteUsageMap usage;
    usage[single.get()].StaticRefCount = 1;
    usage[shared.get()].StaticRefCount = 3;

    auto decisions = ChooseCteReuse(usage);
    EXPECT_EQ(decisions.at(single.get()).Mode, ECteReuseMode::Inline);
    EXPECT_EQ(decisions.at(shared.get()).Mode, ECteReuseMode::Materialize);
}

TEST(CteReuse, SingleReferenceInlines) {
    NQdb::TMockSource t({"a"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto usage = Analyze(BuildSqlPlan("WITH x AS (SELECT a FROM t) SELECT a FROM x", tables));
    EXPECT_EQ(RefCounts(usage), (std::multiset<size_t>{1}));
}

TEST(CteReuse, TwoReferencesMaterialize) {
    NQdb::TMockSource t({"a"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto usage = Analyze(BuildSqlPlan(
        "WITH x AS (SELECT a FROM t) SELECT p.a FROM x p JOIN x q ON p.a = q.a", tables));
    EXPECT_EQ(RefCounts(usage), (std::multiset<size_t>{2}));
}

// Multiplicity through inline/materialized parents is not modeled yet: b is
// referenced twice, a once (inside b), regardless of b's own reuse.
TEST(CteReuse, NestedRefcountsAreSyntactic) {
    NQdb::TMockSource t({"a"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto usage = Analyze(BuildSqlPlan(
        "WITH a AS (SELECT a FROM t), b AS (SELECT a FROM a) "
        "SELECT p.a FROM b p JOIN b q ON p.a = q.a", tables));
    EXPECT_EQ(RefCounts(usage), (std::multiset<size_t>{1, 2}));
}

TEST(CteReuse, ApplyRemovesEveryCteRef) {
    NQdb::TMockSource t({"a"});
    std::map<std::string, ISource*> tables = {{"t", &t}};

    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT a FROM t) SELECT p.a FROM x p JOIN x q ON p.a = q.a", tables);
    auto usage = Analyze(plan);
    auto resolved = ApplyCteReuse(std::move(plan), ChooseCteReuse(usage));
    EXPECT_FALSE(ContainsCteRef(resolved));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
