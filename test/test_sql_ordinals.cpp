#include "mock_source.h"

#include <qdb/plan/build.h>
#include <qdb/catalog/external_module.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/pipeline.h>
#include <qdb/sexp/parser.h>
#include <qdb/sexp/printer.h>
#include <qdb/sql/parser.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include <gtest/gtest.h>

#include <sstream>

using namespace NQdb;

namespace {

std::expected<TOperatorPtr, NQumir::TError> Build(std::string_view text, TMockSource& source) {
    std::istringstream input{std::string(text)};
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto query = parser.Parse(tokens);
    if (!query) {
        return std::unexpected(query.error());
    }
    return BuildPlan(*query, [&](std::string_view name)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::make_shared<TSourceOperator>(source, std::string(name));
    });
}

std::string Failure(std::string_view sql) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TMockSource source(TMockColumns{}, {{"x", i64}, {"y", i64}});
    try {
        auto plan = Build(sql, source);
        if (!plan) {
            return plan.error().ToString();
        }
        ApplyPlanPasses(*plan);
    } catch (const NQumir::TError& error) {
        return error.ToString();
    }
    return {};
}

} // namespace

TEST(SqlOrdinals, RejectsInvalidPositionsAndHiddenOrderColumns) {
    for (const auto& sql : {
        "SELECT x FROM t ORDER BY 0",
        "SELECT x FROM t ORDER BY -1",
        "SELECT x FROM t ORDER BY 2",
        "SELECT x FROM t ORDER BY 9223372036854775807",
        "SELECT x FROM t ORDER BY 2, y",
        "SELECT x FROM t ORDER BY y, 2",
        "SELECT x FROM t GROUP BY 0",
        "SELECT x FROM t GROUP BY -1",
        "SELECT x FROM t GROUP BY 2",
    }) {
        SCOPED_TRACE(sql);
        const auto error = Failure(sql);
        EXPECT_NE(error.find("position"), std::string::npos) << error;
    }
}

TEST(SqlOrdinals, RejectsAggregateWindowAndUnresolvedGroupTargets) {
    for (const auto& sql : {
        "SELECT sum(y) FROM t GROUP BY 1",
        "SELECT rank() OVER (ORDER BY y) FROM t GROUP BY 1",
        "SELECT opaque(x), y FROM t GROUP BY 2",
    }) {
        SCOPED_TRACE(sql);
        const auto error = Failure(sql);
        EXPECT_NE(error.find("GROUP BY position"), std::string::npos) << error;
    }
}

TEST(SqlOrdinals, PositionBindingSurvivesPlanRoundtripAndExcludesHiddenColumns) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TMockSource source(TMockColumns{}, {{"x", i64}, {"y", i64}});
    auto plan = Build("SELECT x AS a FROM t ORDER BY 1 DESC, y", source);
    ASSERT_TRUE(plan) << plan.error().ToString();
    std::ostringstream text;
    NSexp::PrintRelPlan(text, *plan);
    EXPECT_NE(text.str().find("(output 1 1)"), std::string::npos);
    NSexp::TRelParserOptions options;
    options.SourceFactory = [&](std::string_view name, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(source, std::string(name));
    };
    NQumir::NAst::NCore::TParser parser;
    for (auto& [name, callback] : NSexp::MakeRelParsers(std::move(options))) {
        parser.NodeParsers[name] = std::move(callback);
    }
    std::istringstream input(text.str());
    NQumir::NAst::NCore::TTokenStream tokens(input);
    auto parsed = parser.Parse(tokens);
    ASSERT_TRUE(parsed) << parsed.error().ToString();
    auto restored = std::static_pointer_cast<IOperator>(*parsed);
    ApplyPlanPasses(restored);
    auto project = TMaybeOp<TProjectOperator>(restored);
    ASSERT_TRUE(project);
    auto sort = TMaybeOp<TSortOperator>(project.Cast()->Input());
    ASSERT_TRUE(sort);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "a");
    EXPECT_FALSE(sort.Cast()->Keys()[0].OutputPosition);
}

TEST(SqlOrdinals, OrdersFlattenedStructOutputPositionsAfterTyping) {
    TExternalModuleCatalog catalog;
    std::istringstream definitions(
        "CREATE MODULE m LANGUAGE rust AS $$$$;"
        "CREATE FUNCTION pair(DOUBLE) RETURNS (DOUBLE, DOUBLE) "
        "SET MODULE TO m SET SYMBOL TO pair;");
    NSql::TTokenStream tokens(definitions);
    NSql::TParser parser;
    auto statements = parser.ParseAll(tokens);
    ASSERT_TRUE(statements) << statements.error().ToString();
    auto module = NSql::TMaybeNode<NSql::TSqlExternalModule>((*statements)[0]);
    auto function = NSql::TMaybeNode<NSql::TSqlExternalFunction>((*statements)[1]);
    ASSERT_TRUE(module);
    ASSERT_TRUE(function);
    ASSERT_TRUE(catalog.Apply(*module.Cast()));
    ASSERT_TRUE(catalog.Apply(*function.Cast()));
    auto floating = std::make_shared<NQumir::NAst::TFloatType>();
    TMockSource source(TMockColumns{}, {{"x", floating}});
    auto plan = Build("SELECT pair(x), x + 1.0 AS z FROM t ORDER BY 2 DESC, 3", source);
    ASSERT_TRUE(plan) << plan.error().ToString();
    NKernel::TAnnotationContext annotation{.ExternalCatalog = catalog.Snapshot()};
    AnnotateTypes(*plan, annotation);
    auto sort = TMaybeOp<TSortOperator>(*plan);
    ASSERT_TRUE(sort);
    ASSERT_EQ(sort.Cast()->Input()->OutputColumns()->Fields.size(), 3);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "col1");
    EXPECT_EQ(sort.Cast()->Keys()[1].Column, "z");
    auto invalid = Build("SELECT pair(x), x + 1.0 AS z FROM t ORDER BY 4", source);
    ASSERT_TRUE(invalid) << invalid.error().ToString();
    EXPECT_THROW(AnnotateTypes(*invalid, annotation), NQumir::TError);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
