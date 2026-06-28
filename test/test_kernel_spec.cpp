#include <gtest/gtest.h>

#include <qdb/kernel/spec.h>

#include <qumir/parser/type.h>

#include <sstream>

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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
