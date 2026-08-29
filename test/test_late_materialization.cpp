#include <gtest/gtest.h>

#include "mock_source.h"

#include <qdb/plan/build.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/late_materialize.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/pipeline.h>
#include <qdb/sexp/parser.h>
#include <qdb/sexp/printer.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <sstream>

using namespace NQdb;

namespace {

class TLookupMockSource final : public TMockSource, public IRowLookupSource {
public:
    TLookupMockSource()
        : TMockSource(TMockColumns{}, {
            {"URL", std::make_shared<NQumir::NAst::TStringType>()},
            {"EventTime", std::make_shared<NQumir::NAst::TIntegerType>()},
            {"Payload0", std::make_shared<NQumir::NAst::TIntegerType>()},
            {"Payload1", std::make_shared<NQumir::NAst::TIntegerType>()},
            {"Payload2", std::make_shared<NQumir::NAst::TIntegerType>()},
            {"Payload3", std::make_shared<NQumir::NAst::TIntegerType>()},
        })
    {}

    std::optional<TLateMaterializationCost> EstimateLookup(
        std::span<const std::string>,
        std::span<const std::string>,
        uint64_t) const override
    {
        return TLateMaterializationCost{
            .EagerBytes = 10000,
            .NarrowBytes = 500,
            .LookupBytes = 500,
        };
    }

    std::shared_ptr<const IPhysicalRowReader> CompileReader(
        std::span<const std::string>) const override
    {
        return {};
    }
};

std::expected<TOperatorPtr, NQumir::TError> BuildSqlPlan(
    std::string_view sql,
    ISource& source)
{
    std::istringstream input{std::string(sql)};
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return BuildPlan(*parsed, [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::make_shared<TSourceOperator>(source, std::string(table));
    });
}

TPlanPassOptions LateMaterializationOptions(
    TPlanPassDiagnostics* diagnostics = nullptr)
{
    return {
        .EnableCbo = false,
        .LateMaterialization = {.Enabled = true},
        .Diagnostics = diagnostics,
    };
}

std::shared_ptr<TSourceOperator> NarrowSource(const TOperatorPtr& plan) {
    auto late = TMaybeOp<TLateMaterializeOperator>(plan);
    if (!late) return {};
    TOperatorPtr current = late.Cast()->Input();
    if (auto top = TMaybeOp<TTopSortOperator>(current)) {
        current = top.Cast()->Input();
    } else if (auto limit = TMaybeOp<TLimitOperator>(current)) {
        current = limit.Cast()->Input();
    }
    if (auto project = TMaybeOp<TProjectOperator>(current)) {
        current = project.Cast()->Input();
    }
    if (auto filter = TMaybeOp<TFilterOperator>(current)) {
        current = filter.Cast()->Input();
    }
    auto source = TMaybeOp<TSourceOperator>(current);
    return source ? source.Cast() : nullptr;
}

std::shared_ptr<TLateMaterializeOperator> FindLateMaterialize(
    const TOperatorPtr& plan)
{
    if (auto late = TMaybeOp<TLateMaterializeOperator>(plan)) {
        return late.Cast();
    }
    for (const auto& child : plan->Children()) {
        if (auto op = NQumir::NAst::TMaybeNode<IOperator>(child)) {
            if (auto late = FindLateMaterialize(op.Cast())) {
                return late;
            }
        }
    }
    return {};
}

} // namespace

TEST(LateMaterialization, DisabledInPlanPipelineByDefault) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan("SELECT * FROM hits LIMIT 10", source);
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().ToString());

    ApplyPlanPasses(*plan, {.EnableCbo = false});

    EXPECT_FALSE(TMaybeOp<TLateMaterializeOperator>(*plan));
}

TEST(LateMaterialization, RewritesWideFilteredTopK) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan(
        "SELECT * FROM hits WHERE \"URL\" LIKE '%google%' "
        "ORDER BY \"EventTime\" LIMIT 10",
        source);
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().ToString());

    TPlanPassDiagnostics diagnostics;
    ApplyPlanPasses(*plan, LateMaterializationOptions(&diagnostics));

    auto late = TMaybeOp<TLateMaterializeOperator>(*plan);
    ASSERT_TRUE(late);
    EXPECT_EQ(late.Cast()->Columns().size(), 6u);
    EXPECT_EQ(late.Cast()->LocatorColumn(), "__row_id__");
    auto top = TMaybeOp<TTopSortOperator>(late.Cast()->Input());
    ASSERT_TRUE(top);
    auto narrow = TMaybeOp<TProjectOperator>(top.Cast()->Input());
    ASSERT_TRUE(narrow);
    ASSERT_EQ(narrow.Cast()->Projections().size(), 2u);
    EXPECT_EQ(narrow.Cast()->Projections()[0].Name, "EventTime");
    EXPECT_EQ(narrow.Cast()->Projections()[1].Name, "__row_id__");
    auto sourceOp = NarrowSource(*plan);
    ASSERT_TRUE(sourceOp);
    EXPECT_TRUE(sourceOp->EmitsRowId());
    EXPECT_TRUE(diagnostics.LateMaterialization.Applied);
    EXPECT_EQ(diagnostics.LateMaterialization.Limit, 10u);

    auto second = ApplyLateMaterialization(*plan);
    EXPECT_EQ(second.get(), plan->get());
}

TEST(LateMaterialization, RewritesDirectAliasedColumnsWithPlainLimit) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan(
        "SELECT \"URL\" AS u, \"Payload0\" AS p FROM hits LIMIT 10",
        source);
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().ToString());
    ApplyPlanPasses(*plan, LateMaterializationOptions());

    auto late = TMaybeOp<TLateMaterializeOperator>(*plan);
    ASSERT_TRUE(late);
    ASSERT_EQ(late.Cast()->Columns().size(), 2u);
    EXPECT_EQ(late.Cast()->Columns()[0].PhysicalName, "URL");
    EXPECT_EQ(late.Cast()->Columns()[0].OutputName, "u");
    EXPECT_EQ(late.Cast()->Columns()[1].PhysicalName, "Payload0");
    EXPECT_EQ(late.Cast()->Columns()[1].OutputName, "p");
}

TEST(LateMaterialization, FetchesOnlyOutputWhenSortKeyIsHidden) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan(
        "SELECT \"Payload0\" AS p FROM hits "
        "ORDER BY \"EventTime\" LIMIT 10",
        source);
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().ToString());
    ApplyPlanPasses(*plan, LateMaterializationOptions());

    auto late = TMaybeOp<TLateMaterializeOperator>(*plan);
    ASSERT_TRUE(late);
    ASSERT_EQ(late.Cast()->Columns().size(), 1u);
    EXPECT_EQ(late.Cast()->Columns()[0].PhysicalName, "Payload0");
    EXPECT_EQ(late.Cast()->Columns()[0].OutputName, "p");
    auto top = TMaybeOp<TTopSortOperator>(late.Cast()->Input());
    ASSERT_TRUE(top);
    auto narrow = TMaybeOp<TProjectOperator>(top.Cast()->Input());
    ASSERT_TRUE(narrow);
    ASSERT_EQ(narrow.Cast()->Projections().size(), 2u);
    EXPECT_EQ(narrow.Cast()->Projections()[0].Name, "__sort_0");
    auto sortIdent = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(
        narrow.Cast()->Projections()[0].Expression);
    ASSERT_TRUE(sortIdent);
    EXPECT_EQ(sortIdent.Cast()->Name, "hits.EventTime");
    EXPECT_EQ(narrow.Cast()->Projections()[1].Name, "__row_id__");

    TLookupMockSource offsetSource;
    auto offsetPlan = BuildSqlPlan(
        "SELECT \"Payload0\" AS p FROM hits "
        "ORDER BY \"EventTime\" DESC LIMIT 10 OFFSET 5",
        offsetSource);
    ASSERT_TRUE(offsetPlan) << (offsetPlan ? "" : offsetPlan.error().ToString());
    ApplyPlanPasses(*offsetPlan, LateMaterializationOptions());
    auto offsetLate = TMaybeOp<TLateMaterializeOperator>(*offsetPlan);
    ASSERT_TRUE(offsetLate);
    auto offsetLimit = TMaybeOp<TLimitOperator>(offsetLate.Cast()->Input());
    ASSERT_TRUE(offsetLimit);
    EXPECT_EQ(offsetLimit.Cast()->Limit(), 10);
    EXPECT_EQ(offsetLimit.Cast()->Offset(), 5);
}

TEST(LateMaterialization, RejectsLargeLimitAndComputedOutput) {
    TLookupMockSource source;
    auto large = BuildSqlPlan("SELECT * FROM hits LIMIT 101", source);
    ASSERT_TRUE(large);
    ApplyPlanPasses(*large, LateMaterializationOptions());
    EXPECT_FALSE(TMaybeOp<TLateMaterializeOperator>(*large));

    TLookupMockSource computedSource;
    auto computed = BuildSqlPlan(
        "SELECT \"Payload0\" + 1 AS p FROM hits LIMIT 10",
        computedSource);
    ASSERT_TRUE(computed);
    ApplyPlanPasses(*computed, LateMaterializationOptions());
    EXPECT_FALSE(TMaybeOp<TLateMaterializeOperator>(*computed));
}

TEST(LateMaterialization, RejectsReservedUserOutputNames) {
    TLookupMockSource source;
    auto selectAlias = BuildSqlPlan(
        "SELECT \"EventTime\" AS \"__row_id__\" FROM hits", source);
    ASSERT_FALSE(selectAlias);
    EXPECT_NE(
        selectAlias.error().ToString().find("is reserved"),
        std::string::npos);

    auto cteAlias = BuildSqlPlan(
        "WITH x(\"__row_id__\") AS "
        "(SELECT \"EventTime\" FROM hits) SELECT * FROM x",
        source);
    ASSERT_FALSE(cteAlias);
    EXPECT_NE(
        cteAlias.error().ToString().find("is reserved"),
        std::string::npos);
}

TEST(LateMaterialization, AppliesAfterCteOutputPruning) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan(
        "WITH x AS (SELECT * FROM hits ORDER BY \"EventTime\" LIMIT 10) "
        "SELECT \"EventTime\" FROM x",
        source);
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().ToString());

    ApplyPlanPasses(*plan, LateMaterializationOptions());
    auto late = FindLateMaterialize(*plan);
    ASSERT_TRUE(late);
    ASSERT_EQ(late->Columns().size(), 1u);
    EXPECT_EQ(late->Columns()[0].PhysicalName, "EventTime");
    EXPECT_EQ(late->Columns()[0].OutputName, "EventTime");
}

TEST(LateMaterialization, SExpressionRoundTripBindsOutsideParser) {
    TLookupMockSource source;
    auto plan = BuildSqlPlan(
        "SELECT * FROM hits WHERE \"URL\" LIKE '%google%' "
        "ORDER BY \"EventTime\" LIMIT 10",
        source);
    ASSERT_TRUE(plan) << (plan ? "" : plan.error().ToString());
    ApplyPlanPasses(*plan, LateMaterializationOptions());
    ASSERT_TRUE(TMaybeOp<TLateMaterializeOperator>(*plan));

    std::ostringstream serialized;
    NSexp::PrintRelPlan(serialized, *plan);
    EXPECT_NE(
        serialized.str().find("(rel late-materialize"),
        std::string::npos);

    NSexp::TRelParserOptions options;
    options.SourceFactory = [&](std::string_view path, NQumir::TLocation)
        -> TOperatorPtr
    {
        return std::make_shared<TSourceOperator>(source, std::string(path));
    };
    NQumir::NAst::NCore::TParser parser;
    for (auto& [name, parse] : NSexp::MakeRelParsers(std::move(options))) {
        parser.NodeParsers[name] = std::move(parse);
    }
    std::istringstream input(serialized.str());
    NQumir::NAst::NCore::TTokenStream tokens(input);
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed) << (parsed ? "" : parsed.error().ToString());
    auto roundTrip = std::dynamic_pointer_cast<IOperator>(*parsed);
    ASSERT_TRUE(roundTrip);

    auto parsedLate = TMaybeOp<TLateMaterializeOperator>(roundTrip);
    ASSERT_TRUE(parsedLate);
    auto parsedSource = NarrowSource(roundTrip);
    ASSERT_TRUE(parsedSource);
    EXPECT_FALSE(parsedSource->EmitsRowId());

    std::ostringstream serializedAgain;
    NSexp::PrintRelPlan(serializedAgain, roundTrip);
    EXPECT_EQ(serializedAgain.str(), serialized.str());

    ApplyPlanPasses(roundTrip, LateMaterializationOptions());
    EXPECT_TRUE(TMaybeOp<TLateMaterializeOperator>(roundTrip));
    EXPECT_TRUE(parsedSource->EmitsRowId());
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

