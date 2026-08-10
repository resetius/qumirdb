#include <gtest/gtest.h>

#include "mock_source.h"
#include "plan_runner.h"

#include <qdb/catalog/external_module.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/pipeline.h>
#include <qdb/plan/types/nullable.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>

#include <array>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace NQdb;
using namespace NQdb::NSql;
using namespace NQumir::NAst;

namespace {

std::vector<TSqlNodePtr> ParseAll(const std::string& sql) {
    std::istringstream in(sql);
    TTokenStream tokens(in);
    TParser parser;
    auto parsed = parser.ParseAll(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    return std::move(*parsed);
}

void ApplyDefinitions(
    TExternalModuleCatalog& catalog,
    const std::vector<TSqlNodePtr>& statements)
{
    for (const auto& statement : statements) {
        if (auto module = NQdb::NSql::TMaybeNode<TSqlExternalModule>(statement)) {
            auto result = catalog.Apply(*module.Cast());
            ASSERT_TRUE(result) << result.error().ToString();
        } else if (auto function =
                NQdb::NSql::TMaybeNode<TSqlExternalFunction>(statement)) {
            auto result = catalog.Apply(*function.Cast());
            ASSERT_TRUE(result) << result.error().ToString();
        }
    }
}

} // namespace

TEST(ExternalModuleCatalog, CreateIsLazyAndReplaceKeepsFunctions) {
    TExternalModuleCatalog catalog;
    auto first = ParseAll(
        "CREATE MODULE orbital LANGUAGE rust AS $$not valid rust$$;"
        "CREATE FUNCTION distance(DOUBLE) RETURNS DOUBLE "
        "SET MODULE TO orbital SET SYMBOL TO distance;");
    ApplyDefinitions(catalog, first);

    auto replacement = ParseAll(
        "CREATE OR REPLACE MODULE orbital LANGUAGE rust AS $$also deferred$$;");
    ApplyDefinitions(catalog, replacement);

    auto resolved = catalog.Snapshot()->ResolveReturnType(
        "distance", {std::make_shared<TFloatType>()});
    ASSERT_TRUE(resolved) << resolved.error().ToString();
    ASSERT_TRUE(*resolved);
    EXPECT_TRUE(TMaybeType<TFloatType>(**resolved));
}

TEST(ExternalModuleCatalog, RustFailureIsDeferredUntilModuleIsUsed) {
    TExternalModuleCatalog catalog;
    auto statements = ParseAll(
        "CREATE MODULE broken LANGUAGE rust AS $$not valid rust$$;"
        "CREATE FUNCTION broken_fn(DOUBLE) RETURNS DOUBLE "
        "SET MODULE TO broken SET SYMBOL TO broken_fn;");
    ApplyDefinitions(catalog, statements);

    NQumir::TLocation location{};
    auto call = std::make_shared<TCallExpr>(
        location,
        std::make_shared<TIdentExpr>(location, "broken_fn"),
        std::vector<TExprPtr>{std::make_shared<TNumberExpr>(location, 1.0)});
    auto ast = std::make_shared<TBlockExpr>(
        location, std::vector<TExprPtr>{call});
    auto composed = catalog.Snapshot()->ComposeReferenced(ast);
    ASSERT_FALSE(composed);
    EXPECT_NE(composed.error().ToString().find("external module `broken'"),
        std::string::npos);
}

TEST(ExternalModuleCatalog, FunctionsAreOverloadedByArgumentTypes) {
    TExternalModuleCatalog catalog;
    auto statements = ParseAll(
        "CREATE MODULE math LANGUAGE rust AS $$$$;"
        "CREATE FUNCTION convert(DOUBLE) RETURNS DOUBLE "
        "SET MODULE TO math SET SYMBOL TO convert_f64;"
        "CREATE FUNCTION convert(INT) RETURNS INT "
        "SET MODULE TO math SET SYMBOL TO convert_i32;");
    ApplyDefinitions(catalog, statements);

    auto snapshot = catalog.Snapshot();
    auto floating = snapshot->ResolveReturnType(
        "convert", {std::make_shared<TFloatType>()});
    auto integer = snapshot->ResolveReturnType(
        "convert", {std::make_shared<TIntegerType>(TIntegerType::I32)});
    ASSERT_TRUE(floating);
    ASSERT_TRUE(integer);
    ASSERT_TRUE(*floating);
    ASSERT_TRUE(*integer);
    EXPECT_TRUE(TMaybeType<TFloatType>(**floating));
    EXPECT_TRUE(TMaybeType<TIntegerType>(**integer));

    auto duplicate = ParseAll(
        "CREATE FUNCTION convert(INT) RETURNS INT "
        "SET MODULE TO math SET SYMBOL TO another_symbol;");
    auto node = NQdb::NSql::TMaybeNode<TSqlExternalFunction>(duplicate.front());
    ASSERT_TRUE(node);
    EXPECT_FALSE(catalog.Apply(*node.Cast()));
}

TEST(ExternalModuleCatalog, RustFunctionRunsThroughSqlPlan) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    TExternalModuleCatalog catalog;
    auto statements = ParseAll(
        "CREATE MODULE orbital LANGUAGE rust AS $$\n"
        "#[repr(C)]\n"
        "pub struct OrbitVector { pub x: f64, pub y: f64, pub z: f64 }\n"
        "static mut VECTOR_CALLS: u64 = 0;\n"
        "#[no_mangle]\n"
        "pub extern \"C\" fn orbit_distance(x: f64, y: f64) -> f64 { x + y }\n"
        "#[no_mangle]\n"
        "pub extern \"C\" fn orbit_vector(x: f64) -> OrbitVector {\n"
        "    unsafe {\n"
        "        VECTOR_CALLS = VECTOR_CALLS.wrapping_add(1);\n"
        "        let value = x + VECTOR_CALLS as f64;\n"
        "        OrbitVector { x: value, y: value, z: value }\n"
        "    }\n"
        "}\n"
        "$$;"
        "CREATE FUNCTION orbit_distance(DOUBLE, DOUBLE) RETURNS DOUBLE "
        "SET MODULE TO orbital SET SYMBOL TO orbit_distance;"
        "CREATE FUNCTION orbit_vector(DOUBLE) RETURNS (DOUBLE, DOUBLE, DOUBLE) "
        "SET MODULE TO orbital SET SYMBOL TO orbit_vector;"
        "SELECT orbit_distance(x, 2) AS distance, orbit_vector(x), x + 0.0 "
        "FROM points ORDER BY x + 0.0 DESC;");
    ApplyDefinitions(catalog, {statements[0], statements[1], statements[2]});

    NQumir::TLocation location{};
    auto probe = std::make_shared<TBlockExpr>(
        location,
        std::vector<TExprPtr>{std::make_shared<TCallExpr>(
            location,
            std::make_shared<TIdentExpr>(location, "orbit_distance"),
            std::vector<TExprPtr>{
                std::make_shared<TNumberExpr>(location, 1.0),
                std::make_shared<TNumberExpr>(location, 2.0)})});
    auto firstComposition = catalog.Snapshot()->ComposeReferenced(probe);
    ASSERT_TRUE(firstComposition) << firstComposition.error().ToString();
    ASSERT_TRUE(*firstComposition);
    EXPECT_EQ(probe->Stmts.size(), 1);
    auto secondComposition = catalog.Snapshot()->ComposeReferenced(probe);
    ASSERT_TRUE(secondComposition) << secondComposition.error().ToString();
    ASSERT_TRUE(*secondComposition);
    EXPECT_EQ(probe->Stmts.size(), 1);

    std::array<double, 2> values{40.0, 10.5};
    std::array<TColumn, 1> columns{
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    TRowSet batch{
        .Columns = columns.data(),
        .ColumnCount = 1,
        .RowCount = 2,
        .RefCount = 1,
    };
    TMockSource source(
        {"x"}, {std::make_shared<TFloatType>()}, {batch});
    auto plan = BuildPlan(statements[3], [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        if (table != "points") {
            return std::unexpected(NQumir::TError("unknown test table"));
        }
        return std::make_shared<TSourceOperator>(source, std::string(table));
    });
    ASSERT_TRUE(plan) << plan.error().ToString();

    auto sort = TMaybeOp<TSortOperator>(*plan);
    ASSERT_TRUE(sort);
    ASSERT_EQ(sort.Cast()->Keys().size(), 1);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "col2");

    auto snapshot = catalog.Snapshot();
    NKernel::TAnnotationContext annotation{.ExternalCatalog = snapshot};
    AnnotateTypes(*plan, annotation);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "col4");
    ASSERT_TRUE(annotation.Resolver);
    auto resolvedWithCoercion = snapshot->ResolveReturnType(
        "orbit_distance",
        {
            std::make_shared<TFloatType>(),
            std::make_shared<TIntegerType>(TIntegerType::I64),
        });
    ASSERT_TRUE(resolvedWithCoercion) << resolvedWithCoercion.error().ToString();
    ASSERT_TRUE(*resolvedWithCoercion);
    EXPECT_TRUE(TMaybeType<TFloatType>(**resolvedWithCoercion));
    auto* resolver = annotation.Resolver.get();
    AnnotateTypes(*plan, annotation);
    EXPECT_EQ(annotation.Resolver.get(), resolver);
    ApplyPlanPasses(*plan, {
        .EnableCbo = false,
        .Annotation = annotation,
    });
    auto* outputType = static_cast<TStructType*>((*plan)->OutputColumns().get());
    ASSERT_TRUE(outputType);
    ASSERT_EQ(outputType->Fields.size(), 5);
    EXPECT_EQ(outputType->Fields[0].first, "distance");
    EXPECT_EQ(outputType->Fields[1].first, "col1");
    EXPECT_EQ(outputType->Fields[2].first, "col2");
    EXPECT_EQ(outputType->Fields[3].first, "col3");
    EXPECT_EQ(outputType->Fields[4].first, "col4");
    auto runtime = RunPlan(*plan, {}, snapshot);

    TRowSet output{};
    ASSERT_TRUE(runtime->Next(output));
    ASSERT_EQ(output.RowCount, 2);
    ASSERT_EQ(output.ColumnCount, 5);
    const auto* distance = reinterpret_cast<const double*>(output.Columns[0].Data);
    EXPECT_DOUBLE_EQ(distance[0], 42.0);
    EXPECT_DOUBLE_EQ(distance[1], 12.5);
    for (int column = 1; column < 4; ++column) {
        const auto* value = reinterpret_cast<const double*>(output.Columns[column].Data);
        EXPECT_DOUBLE_EQ(value[0], 41.0);
        EXPECT_DOUBLE_EQ(value[1], 12.5);
    }
    const auto* ordered = reinterpret_cast<const double*>(output.Columns[4].Data);
    EXPECT_DOUBLE_EQ(ordered[0], 40.0);
    EXPECT_DOUBLE_EQ(ordered[1], 10.5);
    Release(&output);
    EXPECT_FALSE(runtime->Next(output));
}

TEST(ExternalModuleCatalog, NullableStructProjectionFailsDuringTyping) {
    TExternalModuleCatalog catalog;
    auto statements = ParseAll(
        "CREATE MODULE orbital LANGUAGE rust AS $$$$;"
        "CREATE FUNCTION orbit_vector(DOUBLE) RETURNS (DOUBLE, DOUBLE, DOUBLE) "
        "SET MODULE TO orbital SET SYMBOL TO orbit_vector;"
        "SELECT orbit_vector(x) FROM points;");
    ApplyDefinitions(catalog, {statements[0], statements[1]});

    TMockSource source(
        TMockColumns{},
        {{"x", std::make_shared<TNullable>(std::make_shared<TFloatType>())}});
    auto plan = BuildPlan(statements[2], [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        if (table != "points") {
            return std::unexpected(NQumir::TError("unknown test table"));
        }
        return std::make_shared<TSourceOperator>(source, std::string(table));
    });
    ASSERT_TRUE(plan) << plan.error().ToString();

    try {
        AnnotateTypes(*plan, {
            .ExternalCatalog = catalog.Snapshot(),
        });
        FAIL() << "expected nullable struct projection to fail during typing";
    } catch (const NQumir::TError& error) {
        EXPECT_NE(error.ToString().find(
            "nullable struct-return projections are not supported"),
            std::string::npos);
    }
}

TEST(ExternalModuleCatalog, RustFunctionRunsInFilter) {
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    TExternalModuleCatalog catalog;
    auto statements = ParseAll(
        "CREATE MODULE predicates LANGUAGE rust AS $$\n"
        "#[no_mangle]\n"
        "pub extern \"C\" fn is_large(x: f64, limit: f64) -> bool { x > limit }\n"
        "$$;"
        "CREATE FUNCTION is_large(DOUBLE, DOUBLE) RETURNS BOOL "
        "SET MODULE TO predicates SET SYMBOL TO is_large;"
        "SELECT x FROM points WHERE is_large(x, 10.0);");
    ApplyDefinitions(catalog, {statements[0], statements[1]});

    std::array<double, 4> values{5.0, 10.0, 10.5, 42.0};
    std::array<uint8_t, 1> valid{0b00000111};
    std::array<TColumn, 1> columns{
        TColumn{
            .Data = reinterpret_cast<char*>(values.data()),
            .Mask = valid.data(),
        },
    };
    TRowSet batch{
        .Columns = columns.data(),
        .ColumnCount = 1,
        .RowCount = 4,
        .RefCount = 1,
    };
    TMockSource source(
        {"x"},
        {std::make_shared<NQdb::TNullable>(std::make_shared<TFloatType>())},
        {batch});
    auto plan = BuildPlan(statements[2], [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        if (table != "points") {
            return std::unexpected(NQumir::TError("unknown test table"));
        }
        return std::make_shared<TSourceOperator>(source, std::string(table));
    });
    ASSERT_TRUE(plan) << plan.error().ToString();

    auto snapshot = catalog.Snapshot();
    ApplyPlanPasses(*plan, {
        .EnableCbo = false,
        .Annotation = {.ExternalCatalog = snapshot},
    });
    auto runtime = RunPlan(*plan, {}, snapshot);

    std::vector<double> selected;
    TRowSet output{};
    while (runtime->Next(output)) {
        const auto* data = reinterpret_cast<const double*>(output.Columns[0].Data);
        for (int64_t row = 0; row < output.RowCount; ++row) {
            if (!output.Selection || output.Selection[row]) {
                selected.push_back(data[row]);
            }
        }
        Release(&output);
    }
    EXPECT_EQ(selected, (std::vector<double>{10.5}));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
