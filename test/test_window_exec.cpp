#include <gtest/gtest.h>
#include "mock_source.h"
#include "plan_runner.h"

#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/sexp/parser.h>
#include <qdb/modules/qumirdb_runtime.h>

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

TEST(WindowExec, RankI64WithPeersResetsPerPartition) {
    std::array<int64_t, 7> keys = {2, 1, 1, 2, 1, 2, 1};
    std::array<int64_t, 7> order = {5, 10, 10, 7, 20, 5, 30};
    std::array<int64_t, 7> payload = {100, 1, 2, 200, 3, 101, 4};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{.Data = reinterpret_cast<char*>(payload.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 3,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source({"k", "o", "p"}, std::move(batches));

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(order (o asc nulls-default)) "
        "(fn r rank))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, 7);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outRank = reinterpret_cast<int64_t*>(result.Columns[3].Data);

    const std::array<int64_t, 7> expectedKeys = {1, 1, 1, 1, 2, 2, 2};
    const std::array<int64_t, 7> expectedOrder = {10, 10, 20, 30, 5, 5, 7};
    const std::array<int64_t, 7> expectedRank = {1, 1, 3, 4, 1, 1, 3};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outKeys[row], expectedKeys[row]) << "row " << row;
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(outRank[row], expectedRank[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, RankResetsPerStringAndI64Partition) {
    const std::string groups = "baaabb";
    std::array<int64_t, 7> groupOffsets = {0, 1, 2, 3, 4, 5, 6};
    std::array<int64_t, 6> shards = {2, 1, 1, 1, 2, 1};
    std::array<int64_t, 6> order = {5, 10, 20, 10, 7, 5};
    std::array<int64_t, 6> payload = {200, 1, 3, 2, 300, 100};

    std::vector<TColumn> columns = {
        TColumn{
            .Data = const_cast<char*>(groups.data()),
            .Offsets = groupOffsets.data(),
            .OffsetWidth = 8,
        },
        TColumn{.Data = reinterpret_cast<char*>(shards.data())},
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{.Data = reinterpret_cast<char*>(payload.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 4,
        .RowCount = static_cast<int64_t>(shards.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"g", "s", "o", "p"},
        std::move(batches),
        {
            std::make_shared<NQumir::NAst::TStringType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
            std::make_shared<NQumir::NAst::TIntegerType>(),
        });

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition g s) "
        "(order (o asc nulls-default)) "
        "(fn r rank))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 5);
    ASSERT_EQ(result.RowCount, 6);

    auto* outShard = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outRank = reinterpret_cast<int64_t*>(result.Columns[4].Data);

    ASSERT_EQ(result.Columns[0].OffsetWidth, 8);
    const auto* outOffsets = static_cast<const int64_t*>(result.Columns[0].Offsets);
    const std::string_view outData(result.Columns[0].Data, outOffsets[result.RowCount]);

    const std::array<std::string_view, 6> expectedGroup = {"a", "a", "a", "b", "b", "b"};
    const std::array<int64_t, 6> expectedShard = {1, 1, 1, 1, 2, 2};
    const std::array<int64_t, 6> expectedOrder = {10, 10, 20, 5, 5, 7};
    const std::array<int64_t, 6> expectedRank = {1, 1, 3, 1, 1, 2};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        const std::string_view gotGroup(
            outData.data() + outOffsets[row],
            static_cast<size_t>(outOffsets[row + 1] - outOffsets[row]));
        EXPECT_EQ(gotGroup, expectedGroup[row]) << "row " << row;
        EXPECT_EQ(outShard[row], expectedShard[row]) << "row " << row;
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(outRank[row], expectedRank[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, AvgI64BroadcastsPerPartition) {
    std::array<int64_t, 5> keys = {2, 1, 1, 2, 1};
    std::array<int64_t, 5> values = {10, 3, 6, 30, 12};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source({"k", "v"}, std::move(batches));

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(fn avg_v avg v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 5);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outValues = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outAvg = reinterpret_cast<double*>(result.Columns[2].Data);

    const std::array<int64_t, 5> expectedKeys = {1, 1, 1, 2, 2};
    const std::array<int64_t, 5> expectedValues = {3, 6, 12, 10, 30};
    const std::array<double, 5> expectedAvg = {7.0, 7.0, 7.0, 20.0, 20.0};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outKeys[row], expectedKeys[row]) << "row " << row;
        EXPECT_EQ(outValues[row], expectedValues[row]) << "row " << row;
        EXPECT_DOUBLE_EQ(outAvg[row], expectedAvg[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, AvgDecimalBroadcastsPerPartition) {
    std::array<int64_t, 5> keys = {2, 1, 1, 2, 1};
    std::array<qdb_bin_int, 5> values = {{
        {.Lo = 1000, .Hi = 0},
        {.Lo = 300, .Hi = 0},
        {.Lo = 600, .Hi = 0},
        {.Lo = 3000, .Hi = 0},
        {.Lo = 1200, .Hi = 0},
    }};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"k", "v"},
        std::move(batches),
        {std::make_shared<NQumir::NAst::TIntegerType>(), std::make_shared<TDecimal>(15, 2)});

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(partition k) "
        "(fn avg_v avg v))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 5);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outAvg = reinterpret_cast<qdb_bin_int*>(result.Columns[2].Data);

    const std::array<int64_t, 5> expectedKeys = {1, 1, 1, 2, 2};
    const std::array<uint64_t, 5> expectedAvg = {700, 700, 700, 2000, 2000};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outKeys[row], expectedKeys[row]) << "row " << row;
        EXPECT_EQ(outAvg[row].Lo, expectedAvg[row]) << "row " << row;
        EXPECT_EQ(outAvg[row].Hi, 0u) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, RankI64WithoutPartition) {
    std::array<int64_t, 5> order = {30, 10, 20, 20, 10};
    std::array<int64_t, 5> payload = {3, 1, 2, 4, 5};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{.Data = reinterpret_cast<char*>(payload.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(order.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source({"o", "p"}, std::move(batches));

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(order (o asc nulls-default)) "
        "(fn r rank))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 5);

    auto* outOrder = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outRank = reinterpret_cast<int64_t*>(result.Columns[2].Data);

    const std::array<int64_t, 5> expectedOrder = {10, 10, 20, 20, 30};
    const std::array<int64_t, 5> expectedRank = {1, 1, 3, 3, 5};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outOrder[row], expectedOrder[row]) << "row " << row;
        EXPECT_EQ(outRank[row], expectedRank[row]) << "row " << row;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(WindowExec, RankDecimalWithoutPartition) {
    std::array<qdb_bin_int, 5> order = {{
        {.Lo = 3000, .Hi = 0},
        {.Lo = 1000, .Hi = 0},
        {.Lo = 2000, .Hi = 0},
        {.Lo = 2000, .Hi = 0},
        {.Lo = 1000, .Hi = 0},
    }};
    std::array<int64_t, 5> payload = {3, 1, 2, 4, 5};

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(order.data())},
        TColumn{.Data = reinterpret_cast<char*>(payload.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(order.size()),
        .Selection = nullptr,
        .RefCount = 1,
    }};
    TMockSource source(
        {"d", "p"},
        std::move(batches),
        {std::make_shared<TDecimal>(15, 4), std::make_shared<NQumir::NAst::TIntegerType>()});

    auto root = ParsePlan(
        "(rel window (rel source \"data.parquet\") "
        "(order (d asc nulls-default)) "
        "(fn r rank))",
        source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 5);

    auto* outOrder = reinterpret_cast<qdb_bin_int*>(result.Columns[0].Data);
    auto* outRank = reinterpret_cast<int64_t*>(result.Columns[2].Data);

    const std::array<uint64_t, 5> expectedOrder = {1000, 1000, 2000, 2000, 3000};
    const std::array<int64_t, 5> expectedRank = {1, 1, 3, 3, 5};

    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(outOrder[row].Lo, expectedOrder[row]) << "row " << row;
        EXPECT_EQ(outOrder[row].Hi, 0u) << "row " << row;
        EXPECT_EQ(outRank[row], expectedRank[row]) << "row " << row;
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
