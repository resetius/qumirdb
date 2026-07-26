#include <gtest/gtest.h>
#include "mock_source.h"
#include "plan_runner.h"

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/sexp/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>

#include <array>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace NQdb;

namespace {

TOperatorPtr ParsePlan(const std::string& sexp, ISource& source) {
    NSexp::TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(source, std::string(path));
    };

    NQumir::NAst::NCore::TParser parser;
    for (auto& [name, fn] : NSexp::MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[name] = std::move(fn);
    }

    std::istringstream in(sexp);
    NQumir::NAst::NCore::TTokenStream ts(in);
    auto result = parser.Parse(ts);
    if (!result.has_value()) {
        throw std::runtime_error(result.error().ToString());
    }

    auto root = std::static_pointer_cast<IOperator>(result.value());
    AnnotateTypes(root);
    ApplyColumnPruning(root);
    return root;
}

} // namespace

TEST(WindowExec, PrefixSumI64ResetsPerPartition) {
    std::array<int64_t, 6> keys = {2, 1, 1, 2, 1, 2};
    std::array<int64_t, 6> order = {1, 2, 1, 3, 3, 2};
    std::array<int64_t, 6> values = {20, 7, 5, 4, 11, 6};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 3,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source({"k", "o", "v"}, std::move(batches));

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(order (o asc nulls-default)) "
        "(frame rows (start unbounded-preceding) (end current-row)) "
        "(fn running_sum sum v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, 6);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outValues = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[3].Data);

    const std::array<int64_t, 6> expectedKeys = {1, 1, 1, 2, 2, 2};
    const std::array<int64_t, 6> expectedOrder = {1, 2, 3, 1, 2, 3};
    const std::array<int64_t, 6> expectedValues = {5, 7, 11, 20, 6, 4};
    const std::array<int64_t, 6> expectedSums = {5, 12, 23, 20, 26, 30};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outKeys[row], expectedKeys[row]) << "row " << row;
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(outValues[row], expectedValues[row]) << "row " << row;
        EXPECT_EQ(outSums[row], expectedSums[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
