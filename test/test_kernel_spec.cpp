#include <gtest/gtest.h>

#include <qdb/kernel/builder.h>
#include <qdb/kernel/spec.h>

#include <qumir/parser/type.h>

#include <sstream>
#include <utility>

using namespace NQdb;

TEST(KernelSpec, PrintsStableDebugDescription) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    auto input = std::make_shared<TStructType>(std::vector<std::pair<std::string, TTypePtr>>{
        {"a", i64},
        {"b", i64},
    });
    auto output = std::make_shared<TStructType>(std::vector<std::pair<std::string, TTypePtr>>{
        {"a", i64},
    });

    NKernel::TOperatorKernelSpec spec{
        .Kind = NKernel::EOperatorKernelKind::UnaryStreaming,
        .OperatorName = "filter",
        .InputSchemas = {input},
        .OutputSchema = output,
        .ReferencedColumns = {
            {.Name = "b", .Index = 1, .Type = i64},
        },
        .Keys = {
            {.Name = "predicate-columns",
             .Columns = {{.Name = "b", .Index = 1, .Type = i64}}},
        },
        .Entrypoints = {
            {.Name = "qdb_filter_0", .Abi = "void(ref TRowSet)"},
        },
        .SourceModules = {"qumirdb"},
    };

    std::ostringstream out;
    NKernel::PrintKernelSpec(out, spec);

    EXPECT_EQ(out.str(),
        "kernel-spec filter\n"
        "  kind: unary-streaming\n"
        "  inputs:\n"
        "    [0] struct { a: Int; b: Int; }\n"
        "  output: struct { a: Int; }\n"
        "  referenced:\n"
        "    b#1:i64\n"
        "  keys:\n"
        "    predicate-columns: b#1:i64\n"
        "  entrypoints:\n"
        "    qdb_filter_0: void(ref TRowSet)\n"
        "  source-modules:\n"
        "    qumirdb\n");
}

TEST(KernelSpec, BuildsFilterSpecFromPredicate) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    TStructType input({
        {"a", i64},
        {"b", i64},
        {"c", i64},
    });
    auto predicate = std::make_shared<TBinaryExpr>(
        NQumir::TLocation{}, TOperator("<"),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "c"),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "a"));

    auto spec = NKernel::BuildFilterKernelSpec(input, predicate, "qdb_filter_test");

    EXPECT_EQ(spec.Kind, NKernel::EOperatorKernelKind::UnaryStreaming);
    EXPECT_EQ(spec.OperatorName, "filter");
    ASSERT_EQ(spec.InputSchemas.size(), 1u);
    ASSERT_EQ(spec.ReferencedColumns.size(), 2u);
    EXPECT_EQ(spec.ReferencedColumns[0].Name, "a");
    EXPECT_EQ(spec.ReferencedColumns[0].Index, 0);
    EXPECT_EQ(spec.ReferencedColumns[1].Name, "c");
    EXPECT_EQ(spec.ReferencedColumns[1].Index, 2);
    ASSERT_EQ(spec.Entrypoints.size(), 1u);
    EXPECT_EQ(spec.Entrypoints[0].Name, "qdb_filter_test");
    EXPECT_EQ(spec.Entrypoints[0].Abi, "void(ref TRowSet)");
    ASSERT_EQ(spec.SourceModules.size(), 1u);
    EXPECT_EQ(spec.SourceModules[0], "qumirdb");
}

TEST(KernelSpec, BuildsProjectSpecFromComputedExpressions) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    auto f64 = std::make_shared<TFloatType>();
    TStructType input({
        {"a", i64},
        {"b", f64},
        {"c", i64},
    });
    std::vector<TExprPtr> exprs = {
        std::make_shared<TBinaryExpr>(
            NQumir::TLocation{}, TOperator("+"),
            std::make_shared<TIdentExpr>(NQumir::TLocation{}, "b"),
            std::make_shared<TIdentExpr>(NQumir::TLocation{}, "a")),
    };
    std::vector<TTypePtr> types = {f64};

    auto spec = NKernel::BuildProjectKernelSpec(
        input, exprs, types, "qdb_project_test");

    EXPECT_EQ(spec.Kind, NKernel::EOperatorKernelKind::UnaryStreaming);
    EXPECT_EQ(spec.OperatorName, "project-compute");
    ASSERT_EQ(spec.InputSchemas.size(), 1u);
    ASSERT_EQ(spec.ReferencedColumns.size(), 2u);
    EXPECT_EQ(spec.ReferencedColumns[0].Name, "a");
    EXPECT_EQ(spec.ReferencedColumns[0].Index, 0);
    EXPECT_EQ(spec.ReferencedColumns[1].Name, "b");
    EXPECT_EQ(spec.ReferencedColumns[1].Index, 1);
    auto* output = static_cast<TStructType*>(spec.OutputSchema.get());
    ASSERT_NE(output, nullptr);
    ASSERT_EQ(output->Fields.size(), 1u);
    EXPECT_EQ(output->Fields[0].first, "computed_0");
    EXPECT_EQ(output->Fields[0].second, f64);
    ASSERT_EQ(spec.Entrypoints.size(), 1u);
    EXPECT_EQ(spec.Entrypoints[0].Name, "qdb_project_test");
    EXPECT_EQ(spec.Entrypoints[0].Abi, "void(ref TRowSet, ptr ptr i8)");
    ASSERT_EQ(spec.SourceModules.size(), 1u);
    EXPECT_EQ(spec.SourceModules[0], "qumirdb");
}

TEST(OzFunBuilder, BuildsFunctionDeclaration) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    auto builder = NKernel::NOz::TFunBuilder("identity")
        .Param("value", i64)
        .Return(i64)
        .Stmt(NKernel::NOz::Return(NKernel::NOz::Ident("value")));
    auto ast = std::move(builder).Build();

    auto fun = TMaybeNode<TFunDecl>(ast);
    ASSERT_TRUE(fun);
    EXPECT_EQ(fun.Cast()->Name, "identity");
    ASSERT_EQ(fun.Cast()->Params.size(), 1u);
    EXPECT_EQ(fun.Cast()->Params[0]->Name, "value");
    EXPECT_EQ(fun.Cast()->Params[0]->Type, i64);
    EXPECT_EQ(fun.Cast()->RetType, i64);
    ASSERT_NE(fun.Cast()->Body, nullptr);
    ASSERT_EQ(fun.Cast()->Body->Stmts.size(), 1u);

    auto ret = TMaybeNode<TReturnExpr>(fun.Cast()->Body->Stmts[0]);
    ASSERT_TRUE(ret);
    auto ident = TMaybeNode<TIdentExpr>(ret.Cast()->Value);
    ASSERT_TRUE(ident);
    EXPECT_EQ(ident.Cast()->Name, "value");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
