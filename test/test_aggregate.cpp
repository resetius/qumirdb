#include <gtest/gtest.h>
#include "mock_source.h"

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include "plan_runner.h"
#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>
#include <qdb/modules/qumirdb_runtime.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/sexp/parser.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <algorithm>
#include <array>
#include <memory>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace NQdb;
using namespace NQdb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {

// Parses `sexp` as a (rel aggregate ...) plan whose (rel source "...") leaf is
// backed by `source`, runs AnnotateTypes + ApplyColumnPruning, and returns the
// resulting logical plan root.
TOperatorPtr ParsePlan(const std::string& sexp, ISource& source) {
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(source, std::string(path));
    };

    NQumir::NAst::NCore::TParser parser;
    for (auto& [name, fn] : MakeRelParsers(std::move(opts))) {
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

struct TGroupStats {
    int64_t Count = 0;
    int64_t Sum = 0;
    int64_t Min = 0;
    int64_t Max = 0;
    bool Seen = false;
};

std::unordered_map<int64_t, TGroupStats> ComputeReference(
    const std::vector<int64_t>& keys, const std::vector<int64_t>& vals) {
    std::unordered_map<int64_t, TGroupStats> reference;
    for (size_t i = 0; i < keys.size(); ++i) {
        auto& group = reference[keys[i]];
        if (!group.Seen) {
            group.Min = vals[i];
            group.Max = vals[i];
            group.Seen = true;
        } else {
            group.Min = std::min(group.Min, vals[i]);
            group.Max = std::max(group.Max, vals[i]);
        }
        group.Count += 1;
        group.Sum += vals[i];
    }
    return reference;
}

constexpr const char* kPlanSexp =
    "(rel aggregate (rel source \"data.parquet\") (keys k) "
    "(agg c count) (agg s sum v) (agg mn min v) (agg mx max v))";

bool IsValid(const TColumn& column, int64_t row) {
    if (!column.Mask) {
        return true;
    }
    const int64_t bit = column.MaskBitOffset + row;
    return ((column.Mask[bit / 8] >> (bit % 8)) & 1) != 0;
}

} // namespace

// L6 (multiple groups): full pipeline sexp -> AnnotateTypes -> ApplyColumnPruning
// -> TPhysicalPlanner::Build -> Next(), over an in-memory source with several
// groups split across two batches.
TEST(AggregateE2E, MultipleGroups) {
    std::vector<int64_t> keys = {1, 2, 1, 3, 2, 1, 4, 4, 2, 3, 4, 1};
    std::vector<int64_t> vals = {10, 20, 5, 7, -3, 9, 100, -100, 50, 0, 25, -1};
    ASSERT_EQ(keys.size(), vals.size());

    constexpr size_t batchSize = 6;
    std::vector<std::vector<TColumn>> batchColumns(2);
    std::vector<TRowSet> batches;
    for (size_t b = 0; b < 2; ++b) {
        batchColumns[b] = {
            TColumn{.Data = reinterpret_cast<char*>(keys.data() + b * batchSize)},
            TColumn{.Data = reinterpret_cast<char*>(vals.data() + b * batchSize)},
        };
        batches.push_back(TRowSet{
            .Columns = batchColumns[b].data(),
            .ColumnCount = 2,
            .RowCount = static_cast<int64_t>(batchSize),
            .Selection = nullptr,
            .Destroy = nullptr,
            .Private = nullptr,
            .RefCount = 1,
        });
    }

    NQdb::TMockSource source({"k", "v"}, std::move(batches));
    auto root = ParsePlan(kPlanSexp, source);

    auto runtime = RunPlan(root);

    auto reference = ComputeReference(keys, vals);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 5); // k, c, s, mn, mx
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));
    EXPECT_EQ(result.Columns[0].Mask, nullptr);

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outMins = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* outMaxs = reinterpret_cast<int64_t*>(result.Columns[4].Data);

    for (int64_t i = 0; i < result.RowCount; ++i) {
        const int64_t key = outKeys[i];
        auto it = reference.find(key);
        ASSERT_NE(it, reference.end()) << "unexpected key " << key;
        EXPECT_EQ(outCounts[i], it->second.Count) << "key " << key;
        EXPECT_EQ(outSums[i], it->second.Sum) << "key " << key;
        EXPECT_EQ(outMins[i], it->second.Min) << "key " << key;
        EXPECT_EQ(outMaxs[i], it->second.Max) << "key " << key;
    }

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

// L6 (single group): same pipeline, but every row shares one key — the
// HashTable ends up with exactly one entry.
TEST(AggregateE2E, SingleGroup) {
    std::vector<int64_t> keys = {7, 7, 7, 7, 7, 7};
    std::vector<int64_t> vals = {1, 2, 3, 4, 5, 6};
    ASSERT_EQ(keys.size(), vals.size());

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(vals.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .Destroy = nullptr,
        .Private = nullptr,
        .RefCount = 1,
    }};

    NQdb::TMockSource source({"k", "v"}, std::move(batches));
    auto root = ParsePlan(kPlanSexp, source);

    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 5);
    ASSERT_EQ(result.RowCount, 1);

    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[0].Data)[0], 7);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[1].Data)[0], 6);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[2].Data)[0], 21);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[3].Data)[0], 1);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[4].Data)[0], 6);

    Release(&result);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(AggregateE2E, SumDecimalUsesBinIntState) {
    std::array<int64_t, 5> keys = {1, 1, 2, 2, 1};
    std::array<qdb_bin_int, 5> values = {{
        {.Lo = 1000, .Hi = 0}, // 10.00
        {.Lo = 250, .Hi = 0},  //  2.50
        {.Lo = 3000, .Hi = 0}, // 30.00
        {.Lo = 125, .Hi = 0},  //  1.25
        {.Lo = 75, .Hi = 0},   //  0.75
    }};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 5,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(),
         std::make_shared<NQdb::TDecimal>(7, 2)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 2);
    ASSERT_EQ(result.RowCount, 2);
    EXPECT_EQ(result.Columns[1].Mask, nullptr);
    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* sums = reinterpret_cast<qdb_bin_int*>(result.Columns[1].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        if (outKeys[row] == 1) {
            EXPECT_EQ(sums[row].Lo, 1325u);
            EXPECT_EQ(sums[row].Hi, 0u);
        } else {
            ASSERT_EQ(outKeys[row], 2);
            EXPECT_EQ(sums[row].Lo, 3125u);
            EXPECT_EQ(sums[row].Hi, 0u);
        }
    }
    Release(&result);
}

TEST(AggregateE2E, CompositeIntegerKeysProduceSeparateColumns) {
    struct TPair {
        int64_t First;
        int64_t Second;

        bool operator==(const TPair&) const = default;
    };
    struct TPairHash {
        size_t operator()(const TPair& value) const {
            return std::hash<int64_t>{}(value.First) ^
                (std::hash<int64_t>{}(value.Second) << 1);
        }
    };

    std::vector<int64_t> first = {
        1, 2, 1, 3, 4, 5, 2, 6, 3, 7, 8, 1};
    std::vector<int64_t> second = {
        10, 20, 10, 30, 40, 50, 20, 60, 30, 70, 80, 11};
    std::vector<int64_t> values = {
        5, 7, 11, 13, 17, 19, 3, 23, -2, 29, 31, 37};

    constexpr size_t batchSize = 6;
    std::vector<std::vector<TColumn>> batchColumns(2);
    std::vector<TRowSet> batches;
    for (size_t b = 0; b < 2; ++b) {
        batchColumns[b] = {
            TColumn{.Data = reinterpret_cast<char*>(first.data() + b * batchSize)},
            TColumn{.Data = reinterpret_cast<char*>(second.data() + b * batchSize)},
            TColumn{.Data = reinterpret_cast<char*>(values.data() + b * batchSize)},
        };
        batches.push_back(TRowSet{
            .Columns = batchColumns[b].data(),
            .ColumnCount = 3,
            .RowCount = static_cast<int64_t>(batchSize),
            .Selection = nullptr,
            .Destroy = nullptr,
            .Private = nullptr,
            .RefCount = 1,
        });
    }

    NQdb::TMockSource source({"k1", "k2", "v"}, std::move(batches));
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k1 k2) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    std::unordered_map<TPair, std::pair<int64_t, int64_t>, TPairHash> reference;
    for (size_t i = 0; i < first.size(); ++i) {
        auto& state = reference[{first[i], second[i]}];
        state.first += 1;
        state.second += values[i];
    }

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4); // k1, k2, count, sum
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));
    auto* outFirst = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outSecond = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    for (int64_t i = 0; i < result.RowCount; ++i) {
        auto it = reference.find({outFirst[i], outSecond[i]});
        ASSERT_NE(it, reference.end());
        EXPECT_EQ(outCounts[i], it->second.first);
        EXPECT_EQ(outSums[i], it->second.second);
    }
    Release(&result);
}

TEST(AggregateE2E, ScalarI32KeyPreservesTypedOutput) {
    std::vector<int32_t> keys = {-1, 2, -1, 3, 4, 5, 2, 6, 3, 7, 8, -1};
    std::vector<int64_t> values = {5, 7, 11, 13, 17, 19, 3, 23, -2, 29, 31, 37};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .Destroy = nullptr,
        .Private = nullptr,
        .RefCount = 1,
    }};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(TIntegerType::I32),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    std::unordered_map<int32_t, std::pair<int64_t, int64_t>> reference;
    for (size_t i = 0; i < keys.size(); ++i) {
        auto& state = reference[keys[i]];
        state.first += 1;
        state.second += values[i];
    }

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));
    auto* outKeys = reinterpret_cast<int32_t*>(result.Columns[0].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    for (int64_t i = 0; i < result.RowCount; ++i) {
        auto it = reference.find(outKeys[i]);
        ASSERT_NE(it, reference.end());
        EXPECT_EQ(outCounts[i], it->second.first);
        EXPECT_EQ(outSums[i], it->second.second);
    }
    Release(&result);
}

TEST(AggregateE2E, ScalarStringKeyOwnsFinalizedOutput) {
    std::string data1 = "alphabetaalpha";
    std::array<int32_t, 4> offsets1 = {0, 5, 9, 14};
    std::array<int64_t, 3> values1 = {10, 20, 5};
    std::array<uint8_t, 3> selection1 = {1, 0, 1};
    std::string data2 = "betaмир";
    std::array<int64_t, 4> offsets2 = {
        0, 4, 4, static_cast<int64_t>(data2.size())};
    std::array<int64_t, 3> values2 = {-3, 11, 7};

    std::vector<std::vector<TColumn>> batchColumns(2);
    batchColumns[0] = {
        TColumn{
            .Data = data1.data(), .Mask = nullptr,
            .Offsets = offsets1.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values1.data())},
    };
    batchColumns[1] = {
        TColumn{
            .Data = data2.data(), .Mask = nullptr,
            .Offsets = offsets2.data(), .OffsetWidth = 8},
        TColumn{.Data = reinterpret_cast<char*>(values2.data())},
    };
    std::vector<TRowSet> batches = {
        TRowSet{
            .Columns = batchColumns[0].data(), .ColumnCount = 2, .RowCount = 3,
            .Selection = selection1.data(), .RefCount = 1},
        TRowSet{
            .Columns = batchColumns[1].data(), .ColumnCount = 2, .RowCount = 3,
            .Selection = nullptr, .RefCount = 1},
    };
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TStringType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, 4);
    ASSERT_EQ(result.Columns[0].OffsetWidth, 8);
    auto* outputOffsets = static_cast<int64_t*>(result.Columns[0].Offsets);
    auto* counts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    std::map<std::string, std::pair<int64_t, int64_t>> actual;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        std::string key(result.Columns[0].Data + outputOffsets[row],
            result.Columns[0].Data + outputOffsets[row + 1]);
        actual[key] = {counts[row], sums[row]};
    }
    EXPECT_EQ(actual.at("alpha"), (std::pair<int64_t, int64_t>{2, 15}));
    EXPECT_EQ(actual.at("beta"), (std::pair<int64_t, int64_t>{1, -3}));
    EXPECT_EQ(actual.at(""), (std::pair<int64_t, int64_t>{1, 11}));
    EXPECT_EQ(actual.at("мир"), (std::pair<int64_t, int64_t>{1, 7}));
    Release(&result);
}

TEST(AggregateE2E, NullStringKeysGroupTogetherAndDifferFromEmpty) {
    std::string data = "hidden-onehidden-two";
    std::array<int32_t, 5> offsets = {0, 10, 20, 20, 20};
    std::array<uint8_t, 1> mask = {0b00001100};
    std::array<int64_t, 4> values = {10, 20, 3, 4};
    std::vector<TColumn> columns = {
        TColumn{.Data = data.data(), .Mask = mask.data(),
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 4,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TNullable>(std::make_shared<TStringType>()),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 2);
    auto* counts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outputOffsets = static_cast<int64_t*>(result.Columns[0].Offsets);
    std::map<int64_t, int64_t> countBySum;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        countBySum[sums[row]] = counts[row];
        const std::string key(result.Columns[0].Data + outputOffsets[row],
            result.Columns[0].Data + outputOffsets[row + 1]);
        EXPECT_TRUE(key.empty());
        EXPECT_EQ(IsValid(result.Columns[0], row), sums[row] == 7);
    }
    EXPECT_EQ(countBySum.at(30), 2);
    EXPECT_EQ(countBySum.at(7), 2);
    Release(&result);
}

TEST(AggregateE2E, NullIntegerKeysIgnorePayloadAndDifferFromZero) {
    std::array<int64_t, 4> keys = {41, 99, 0, 0};
    std::array<uint8_t, 1> mask = {0b00001100};
    std::array<int64_t, 4> values = {10, 20, 3, 4};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data()), .Mask = mask.data()},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 4,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TNullable>(
             std::make_shared<TIntegerType>(TIntegerType::I64)),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 2);
    auto* counts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outputKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    std::map<int64_t, int64_t> countBySum;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        countBySum[sums[row]] = counts[row];
        EXPECT_EQ(outputKeys[row], 0);
        EXPECT_EQ(IsValid(result.Columns[0], row), sums[row] == 7);
    }
    EXPECT_EQ(countBySum.at(30), 2);
    EXPECT_EQ(countBySum.at(7), 2);
    Release(&result);
}

TEST(AggregateE2E, CompositeNullKeysTrackValidityPerPosition) {
    std::array<int64_t, 6> first = {41, 99, 0, 17, 23, 88};
    std::array<int64_t, 6> second = {1, 1, 1, 2, 31, 77};
    std::array<uint8_t, 1> firstMask = {0b00000100};
    std::array<uint8_t, 1> secondMask = {0b00001111};
    std::array<int64_t, 6> values = {10, 20, 3, 4, 5, 6};
    std::vector<TColumn> columns = {
        TColumn{
            .Data = reinterpret_cast<char*>(first.data()),
            .Mask = firstMask.data()},
        TColumn{
            .Data = reinterpret_cast<char*>(second.data()),
            .Mask = secondMask.data()},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 3, .RowCount = 6,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k1", "k2", "v"}, std::move(batches),
        {std::make_shared<TNullable>(std::make_shared<TIntegerType>()),
         std::make_shared<TNullable>(std::make_shared<TIntegerType>()),
         std::make_shared<TIntegerType>()});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k1 k2) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 4);
    auto* counts = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    std::map<int64_t, int64_t> countBySum;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        countBySum[sums[row]] = counts[row];
        const bool firstValid = IsValid(result.Columns[0], row);
        const bool secondValid = IsValid(result.Columns[1], row);
        if (sums[row] == 3) {
            EXPECT_TRUE(firstValid);
            EXPECT_TRUE(secondValid);
        } else if (sums[row] == 11) {
            EXPECT_FALSE(firstValid);
            EXPECT_FALSE(secondValid);
        } else {
            EXPECT_FALSE(firstValid);
            EXPECT_TRUE(secondValid);
        }
    }
    EXPECT_EQ(countBySum.at(30), 2);
    EXPECT_EQ(countBySum.at(3), 1);
    EXPECT_EQ(countBySum.at(4), 1);
    EXPECT_EQ(countBySum.at(11), 2);
    Release(&result);
}

TEST(AggregateE2E, MixedStringI32KeysFinalizeToSeparateColumns) {
    std::array<int32_t, 4> ids1 = {1, 1, 2, 3};
    std::string data1 = "alphaalphabetagamma";
    std::array<int32_t, 5> offsets1 = {0, 5, 10, 14, 19};
    std::array<int64_t, 4> values1 = {10, 5, 20, 100};
    std::array<uint8_t, 4> selection1 = {1, 1, 1, 0};
    std::array<int32_t, 3> ids2 = {2, 4, 1};
    std::string data2 = "betadeltaalpha";
    std::array<int32_t, 4> offsets2 = {0, 4, 9, 14};
    std::array<int64_t, 3> values2 = {-3, 11, 7};

    std::vector<std::vector<TColumn>> columns(2);
    columns[0] = {
        TColumn{.Data = reinterpret_cast<char*>(ids1.data())},
        TColumn{.Data = data1.data(), .Mask = nullptr,
            .Offsets = offsets1.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values1.data())},
    };
    columns[1] = {
        TColumn{.Data = reinterpret_cast<char*>(ids2.data())},
        TColumn{.Data = data2.data(), .Mask = nullptr,
            .Offsets = offsets2.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values2.data())},
    };
    std::vector<TRowSet> batches = {
        TRowSet{.Columns = columns[0].data(), .ColumnCount = 3, .RowCount = 4,
            .Selection = selection1.data(), .RefCount = 1},
        TRowSet{.Columns = columns[1].data(), .ColumnCount = 3, .RowCount = 3,
            .Selection = nullptr, .RefCount = 1},
    };
    NQdb::TMockSource source(
        {"id", "name", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(TIntegerType::I32),
         std::make_shared<TStringType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys name id) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, 3);
    auto* outOffsets = static_cast<int64_t*>(result.Columns[0].Offsets);
    auto* outIds = reinterpret_cast<int32_t*>(result.Columns[1].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    std::map<std::pair<int64_t, std::string>,
        std::pair<int64_t, int64_t>> actual;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        std::string name(result.Columns[0].Data + outOffsets[row],
            result.Columns[0].Data + outOffsets[row + 1]);
        actual[{outIds[row], name}] = {outCounts[row], outSums[row]};
    }
    EXPECT_EQ(actual.at({1, "alpha"}),
        (std::pair<int64_t, int64_t>{3, 22}));
    EXPECT_EQ(actual.at({2, "beta"}),
        (std::pair<int64_t, int64_t>{2, 17}));
    EXPECT_EQ(actual.at({4, "delta"}),
        (std::pair<int64_t, int64_t>{1, 11}));
    Release(&result);
}

TEST(AggregateE2E, MixedI32StringKeysPreserveReducersAcrossGrow) {
    constexpr int32_t rowCount = 40;
    std::vector<int32_t> ids(rowCount);
    std::string names;
    std::vector<int32_t> offsets(rowCount + 1);
    std::vector<int64_t> values(rowCount);
    std::map<std::pair<int32_t, std::string>,
        std::pair<int64_t, int64_t>> expected;
    for (int32_t row = 0; row < rowCount; ++row) {
        ids[row] = row % 23;
        const std::string name = "status_" + std::to_string(row % 3);
        names += name;
        offsets[row + 1] = static_cast<int32_t>(names.size());
        values[row] = row + 1;
        auto& state = expected[{ids[row], name}];
        ++state.first;
        state.second += values[row];
    }

    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(ids.data())},
        TColumn{.Data = names.data(), .Mask = nullptr,
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 3, .RowCount = rowCount,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"id", "name", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(TIntegerType::I32),
         std::make_shared<TStringType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys id name) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(expected.size()));
    auto* outIds = reinterpret_cast<int32_t*>(result.Columns[0].Data);
    auto* outOffsets = static_cast<int64_t*>(result.Columns[1].Offsets);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        std::string name(result.Columns[1].Data + outOffsets[row],
            result.Columns[1].Data + outOffsets[row + 1]);
        EXPECT_EQ((std::pair<int64_t, int64_t>{outCounts[row], outSums[row]}),
            expected.at({outIds[row], name}));
    }
    Release(&result);
}

TEST(AggregateE2E, TwoStringKeysFinalizeIndependentColumns) {
    std::string leftData = "ababa";
    std::string rightData = "xyxzy";
    std::array<int32_t, 6> offsets = {0, 1, 2, 3, 4, 5};
    std::array<int64_t, 5> values = {10, 20, 5, 7, 11};
    std::vector<TColumn> columns = {
        TColumn{.Data = leftData.data(), .Mask = nullptr,
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = rightData.data(), .Mask = nullptr,
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 3, .RowCount = 5,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"left", "right", "v"}, std::move(batches),
        {std::make_shared<TStringType>(), std::make_shared<TStringType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys left right) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, 4);
    ASSERT_EQ(result.Columns[0].OffsetWidth, 8);
    ASSERT_EQ(result.Columns[1].OffsetWidth, 8);
    auto* leftOffsets = static_cast<int64_t*>(result.Columns[0].Offsets);
    auto* rightOffsets = static_cast<int64_t*>(result.Columns[1].Offsets);
    auto* counts = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    std::map<std::pair<std::string, std::string>,
        std::pair<int64_t, int64_t>> actual;
    for (int64_t row = 0; row < result.RowCount; ++row) {
        std::string left(result.Columns[0].Data + leftOffsets[row],
            result.Columns[0].Data + leftOffsets[row + 1]);
        std::string right(result.Columns[1].Data + rightOffsets[row],
            result.Columns[1].Data + rightOffsets[row + 1]);
        actual[{left, right}] = {counts[row], sums[row]};
    }
    EXPECT_EQ(actual.at({"a", "x"}),
        (std::pair<int64_t, int64_t>{2, 15}));
    EXPECT_EQ(actual.at({"b", "y"}),
        (std::pair<int64_t, int64_t>{1, 20}));
    EXPECT_EQ(actual.at({"b", "z"}),
        (std::pair<int64_t, int64_t>{1, 7}));
    EXPECT_EQ(actual.at({"a", "y"}),
        (std::pair<int64_t, int64_t>{1, 11}));
    Release(&result);
}

TEST(AggregateE2E, ScalarF64KeyCanonicalizesSignedZero) {
    const double nan1 = std::numeric_limits<double>::quiet_NaN();
    const double nan2 = std::bit_cast<double>(UINT64_C(0x7ff0000000000001));
    std::vector<double> keys = {
        0.0, -0.0, 1.5, -2.25, 3.0, 4.5,
        1.5, 5.75, -2.25, 6.0, nan1, nan2};
    std::vector<int64_t> values = {
        5, 7, 11, 13, 17, 19, 3, 23, -2, 29, 31, 37};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = static_cast<int64_t>(keys.size()),
        .Selection = nullptr,
        .Destroy = nullptr,
        .Private = nullptr,
        .RefCount = 1,
    }};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TFloatType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    auto canonicalBits = [](double value) {
        uint64_t bits = std::bit_cast<uint64_t>(value);
        constexpr uint64_t signMask = UINT64_C(0x8000000000000000);
        constexpr uint64_t exponentMask = UINT64_C(0x7ff0000000000000);
        constexpr uint64_t fractionMask = UINT64_C(0x000fffffffffffff);
        if ((bits & ~signMask) == 0) {
            return UINT64_C(0);
        }
        if ((bits & exponentMask) == exponentMask &&
            (bits & fractionMask) != 0) {
            return UINT64_C(0x7ff8000000000000);
        }
        return bits;
    };
    std::map<uint64_t, std::pair<int64_t, int64_t>> reference;
    for (size_t i = 0; i < keys.size(); ++i) {
        auto& state = reference[canonicalBits(keys[i])];
        state.first += 1;
        state.second += values[i];
    }

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));
    auto* outKeys = reinterpret_cast<double*>(result.Columns[0].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    for (int64_t i = 0; i < result.RowCount; ++i) {
        auto it = reference.find(canonicalBits(outKeys[i]));
        ASSERT_NE(it, reference.end());
        EXPECT_EQ(outCounts[i], it->second.first);
        EXPECT_EQ(outSums[i], it->second.second);
    }
    Release(&result);
}

TEST(AggregateE2E, MixedI32F64CompositeKeyPreservesLayoutAndTypedColumns) {
    std::vector<int32_t> first = {1, 1, 2, 2, 3, 4, 1, 5, 3, 6, 7, 1};
    std::vector<double> second = {
        0.0, -0.0, 407986.23, 417231.63, -3.25, 4.0,
        1.01, 5.5, -3.25, 6.0, 7.0, 1.01};
    std::vector<int64_t> values = {5, 7, 11, 13, 17, 19, 3, 23, -2, 29, 31, 37};

    constexpr size_t batchSize = 6;
    std::vector<std::vector<TColumn>> batchColumns(2);
    std::vector<TRowSet> batches;
    for (size_t b = 0; b < 2; ++b) {
        batchColumns[b] = {
            TColumn{.Data = reinterpret_cast<char*>(first.data() + b * batchSize)},
            TColumn{.Data = reinterpret_cast<char*>(second.data() + b * batchSize)},
            TColumn{.Data = reinterpret_cast<char*>(values.data() + b * batchSize)},
        };
        batches.push_back(TRowSet{
            .Columns = batchColumns[b].data(),
            .ColumnCount = 3,
            .RowCount = static_cast<int64_t>(batchSize),
            .Selection = nullptr,
            .Destroy = nullptr,
            .Private = nullptr,
            .RefCount = 1,
        });
    }
    NQdb::TMockSource source(
        {"k1", "k2", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(TIntegerType::I32),
         std::make_shared<TFloatType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k1 k2) "
        "(agg c count) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    std::map<std::pair<int32_t, double>, std::pair<int64_t, int64_t>> reference;
    for (size_t i = 0; i < first.size(); ++i) {
        auto& state = reference[{first[i], second[i]}];
        state.first += 1;
        state.second += values[i];
    }

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 4);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));
    auto* outFirst = reinterpret_cast<int32_t*>(result.Columns[0].Data);
    auto* outSecond = reinterpret_cast<double*>(result.Columns[1].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    for (int64_t i = 0; i < result.RowCount; ++i) {
        auto it = reference.find({outFirst[i], outSecond[i]});
        ASSERT_NE(it, reference.end());
        EXPECT_EQ(outCounts[i], it->second.first);
        EXPECT_EQ(outSums[i], it->second.second);
    }
    Release(&result);
}

TEST(AggregateE2E, PlannerUsesPhysicalPrunedSchemaForKeyDescriptor) {
    class TPruningSource final : public ISource {
    public:
        TPruningSource() {
            OriginalColumns_ = {
                {"unused", std::make_shared<TStringType>()},
                {"key_i32", std::make_shared<TIntegerType>(TIntegerType::I32)},
                {"value", std::make_shared<TIntegerType>(TIntegerType::I64)},
                {"key_f64", std::make_shared<TFloatType>()},
            };
            PhysicalColumns_ = {
                {"value", std::make_shared<TIntegerType>(TIntegerType::I64)},
                {"key_f64", std::make_shared<TFloatType>()},
                {"key_i32", std::make_shared<TIntegerType>(TIntegerType::I32)},
            };
            CurrentSchema_ = TSchema{OriginalColumns_};
        }

        const TSchema& Schema() const override {
            return CurrentSchema_;
        }

        const TStatsPtr Stats() const override {
            return nullptr;
        }

        void RestrictColumns(const std::unordered_set<std::string>& names) override {
            RestrictedColumns_ = names;
            CurrentSchema_ = TSchema{PhysicalColumns_};
        }

        bool Next(TRowSet& rowSet) override {
            if (Done_) {
                return false;
            }
            Columns_[0].Data = reinterpret_cast<char*>(Values_.data());
            Columns_[1].Data = reinterpret_cast<char*>(F64Keys_.data());
            Columns_[2].Data = reinterpret_cast<char*>(I32Keys_.data());
            rowSet = TRowSet{
                .Columns = Columns_.data(),
                .ColumnCount = static_cast<int64_t>(Columns_.size()),
                .RowCount = static_cast<int64_t>(Values_.size()),
                .Selection = nullptr,
                .Destroy = nullptr,
                .Private = nullptr,
                .RefCount = 1,
            };
            Done_ = true;
            return true;
        }

        const std::unordered_set<std::string>& RestrictedColumns() const {
            return RestrictedColumns_;
        }

    private:
        std::vector<TColumnSchema> OriginalColumns_;
        std::vector<TColumnSchema> PhysicalColumns_;
        TSchema CurrentSchema_{};
        std::unordered_set<std::string> RestrictedColumns_;
        std::vector<int64_t> Values_ = {5, 7, 11, 13, -2, 17};
        std::vector<double> F64Keys_ = {1.25, 2.5, 1.25, 3.75, 2.5, 4.125};
        std::vector<int32_t> I32Keys_ = {10, 20, 10, 30, 20, 40};
        std::array<TColumn, 3> Columns_{};
        bool Done_ = false;
    } source;

    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") "
        "(keys key_i32 key_f64) (agg s sum value))",
        source);
    auto runtime = RunPlan(root);

    EXPECT_EQ(source.RestrictedColumns(),
        (std::unordered_set<std::string>{"key_i32", "key_f64", "value"}));
    auto outputType = TMaybeType<TStructType>(runtime->OutputType());
    ASSERT_TRUE(outputType);
    ASSERT_EQ(outputType.Cast()->Fields.size(), 3u);
    EXPECT_TRUE(TMaybeType<TIntegerType>(outputType.Cast()->Fields[0].second));
    EXPECT_TRUE(TMaybeType<TFloatType>(outputType.Cast()->Fields[1].second));

    std::map<std::pair<int32_t, double>, int64_t> expected = {
        {{10, 1.25}, 16}, {{20, 2.5}, 5},
        {{30, 3.75}, 13}, {{40, 4.125}, 17},
    };
    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 3);
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(expected.size()));
    auto* outI32 = reinterpret_cast<int32_t*>(result.Columns[0].Data);
    auto* outF64 = reinterpret_cast<double*>(result.Columns[1].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    for (int64_t i = 0; i < result.RowCount; ++i) {
        auto it = expected.find({outI32[i], outF64[i]});
        ASSERT_NE(it, expected.end());
        EXPECT_EQ(outSums[i], it->second);
    }
    Release(&result);
}

TEST(AggregateE2E, CompilesNullableReducerArgumentWithUnwrappedAstType) {
    TStructType inputType({
        {"k", std::make_shared<TIntegerType>()},
        {"v", std::make_shared<TNullable>(std::make_shared<TIntegerType>())},
    });
    NQumir::TLocation loc{};
    std::vector<NQdb::TAggregateSpec> aggs = {{
        .Name = "s",
        .Func = "sum",
        .Arg = std::make_shared<TIdentExpr>(loc, "v"),
    }};
    auto spec = NQdb::NKernel::BuildAggregateKernelSpec(inputType, {"k"}, aggs);
    EXPECT_NO_THROW(
        NQdb::TKernelCompiler().CompileAggregate(spec));
}

// M13.8: a nullable reducer argument must distinguish count(*) (every row) from
// count(arg)/sum/min/max (only non-null arguments). A group whose argument is
// NULL in every row produces a NULL sum/min/max (empty-input semantics) but a
// non-null count(*).
TEST(AggregateE2E, NullableReducerArgumentSeparatesCountStarFromCountArg) {
    // k=1: v in {10, NULL, 4}; k=2: all NULL; k=3: v in {7}.
    std::array<int64_t, 6> keys = {1, 1, 1, 2, 2, 3};
    std::array<int64_t, 6> values = {10, 999, 4, 888, 777, 7};
    // valid (non-null) at rows 0, 2, 5.
    std::array<uint8_t, 1> mask = {0b00100101};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data()),
            .Mask = mask.data()},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 6,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(),
         std::make_shared<TNullable>(std::make_shared<TIntegerType>())});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg cn count v) (agg s sum v) "
        "(agg mn min v) (agg mx max v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 6); // k, c, cn, s, mn, mx
    ASSERT_EQ(result.RowCount, 3);
    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* countStar = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* countArg = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* mins = reinterpret_cast<int64_t*>(result.Columns[4].Data);
    auto* maxs = reinterpret_cast<int64_t*>(result.Columns[5].Data);
    // count(*) and count(arg) are always non-nullable.
    EXPECT_EQ(result.Columns[1].Mask, nullptr);
    EXPECT_EQ(result.Columns[2].Mask, nullptr);

    for (int64_t row = 0; row < result.RowCount; ++row) {
        const int64_t key = outKeys[row];
        const bool sumValid = IsValid(result.Columns[3], row);
        const bool minValid = IsValid(result.Columns[4], row);
        const bool maxValid = IsValid(result.Columns[5], row);
        if (key == 1) {
            EXPECT_EQ(countStar[row], 3);
            EXPECT_EQ(countArg[row], 2);
            EXPECT_EQ(sums[row], 14);
            EXPECT_EQ(mins[row], 4);
            EXPECT_EQ(maxs[row], 10);
            EXPECT_TRUE(sumValid);
            EXPECT_TRUE(minValid);
            EXPECT_TRUE(maxValid);
        } else if (key == 2) {
            EXPECT_EQ(countStar[row], 2);
            EXPECT_EQ(countArg[row], 0);
            EXPECT_FALSE(sumValid); // all-null group -> NULL aggregate output
            EXPECT_FALSE(minValid);
            EXPECT_FALSE(maxValid);
        } else {
            ASSERT_EQ(key, 3);
            EXPECT_EQ(countStar[row], 1);
            EXPECT_EQ(countArg[row], 1);
            EXPECT_EQ(sums[row], 7);
            EXPECT_EQ(mins[row], 7);
            EXPECT_EQ(maxs[row], 7);
            EXPECT_TRUE(sumValid);
            EXPECT_TRUE(minValid);
            EXPECT_TRUE(maxValid);
        }
    }
    Release(&result);
}

// A row whose argument is NULL must not contribute even when it is selected, and
// an unselected row must not contribute regardless of its argument validity.
TEST(AggregateE2E, NullableReducerArgumentHonoursSelectionMask) {
    // All rows share key 1. Selected: 0, 3, 4 (row 4 has a NULL argument).
    std::array<int64_t, 5> keys = {1, 1, 1, 1, 1};
    std::array<int64_t, 5> values = {10, 999, 777, 5, 888};
    std::array<uint8_t, 1> mask = {0b00001011}; // valid at rows 0, 1, 3
    std::array<uint8_t, 5> selection = {1, 0, 0, 1, 1};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data()),
            .Mask = mask.data()},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 5,
        .Selection = selection.data(), .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(),
         std::make_shared<TNullable>(std::make_shared<TIntegerType>())});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg cn count v) (agg s sum v) "
        "(agg mn min v) (agg mx max v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 1);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[1].Data)[0], 3); // count(*)
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[2].Data)[0], 2); // count(v)
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[3].Data)[0], 15); // 10 + 5
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[4].Data)[0], 5);
    EXPECT_EQ(reinterpret_cast<int64_t*>(result.Columns[5].Data)[0], 10);
    EXPECT_TRUE(IsValid(result.Columns[3], 0));
    EXPECT_TRUE(IsValid(result.Columns[4], 0));
    EXPECT_TRUE(IsValid(result.Columns[5], 0));
    Release(&result);
}

// Valid-count buffers must accumulate across multiple update batches, including
// through a hash-table grow triggered by exceeding the initial capacity.
TEST(AggregateE2E, NullableReducerArgumentAccumulatesAcrossBatchesAndGrow) {
    constexpr int64_t kBatches = 3;
    constexpr int64_t kPerBatch = 8;
    std::vector<std::vector<int64_t>> keyStore(kBatches);
    std::vector<std::vector<int64_t>> valStore(kBatches);
    std::vector<std::vector<uint8_t>> maskStore(kBatches);
    std::vector<std::vector<TColumn>> columnStore(kBatches);
    std::vector<TRowSet> batches;

    struct TRef {
        int64_t CountStar = 0;
        int64_t CountArg = 0;
        int64_t Sum = 0;
        int64_t Min = 0;
        int64_t Max = 0;
        bool Seen = false;
    };
    std::map<int64_t, TRef> reference;

    for (int64_t b = 0; b < kBatches; ++b) {
        keyStore[b].resize(kPerBatch);
        valStore[b].resize(kPerBatch);
        maskStore[b].assign((kPerBatch + 7) / 8, 0);
        for (int64_t i = 0; i < kPerBatch; ++i) {
            const int64_t row = b * kPerBatch + i;
            const int64_t key = row % 10; // 10 distinct keys forces a grow
            const int64_t value = row + 1;
            const bool valid = (row % 3 != 0);
            keyStore[b][i] = key;
            valStore[b][i] = valid ? value : -123456; // poison ignored values
            if (valid) {
                maskStore[b][i / 8] |= (1u << (i % 8));
            }
            auto& ref = reference[key];
            ref.CountStar += 1;
            if (valid) {
                if (!ref.Seen) {
                    ref.Min = value;
                    ref.Max = value;
                    ref.Seen = true;
                } else {
                    ref.Min = std::min(ref.Min, value);
                    ref.Max = std::max(ref.Max, value);
                }
                ref.CountArg += 1;
                ref.Sum += value;
            }
        }
        columnStore[b] = {
            TColumn{.Data = reinterpret_cast<char*>(keyStore[b].data())},
            TColumn{.Data = reinterpret_cast<char*>(valStore[b].data()),
                .Mask = maskStore[b].data()},
        };
        batches.push_back(TRowSet{
            .Columns = columnStore[b].data(), .ColumnCount = 2,
            .RowCount = kPerBatch, .Selection = nullptr, .RefCount = 1});
    }

    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(),
         std::make_shared<TNullable>(std::make_shared<TIntegerType>())});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg cn count v) (agg s sum v) "
        "(agg mn min v) (agg mx max v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));
    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* countStar = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* countArg = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* mins = reinterpret_cast<int64_t*>(result.Columns[4].Data);
    auto* maxs = reinterpret_cast<int64_t*>(result.Columns[5].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        const auto it = reference.find(outKeys[row]);
        ASSERT_NE(it, reference.end());
        const auto& ref = it->second;
        EXPECT_EQ(countStar[row], ref.CountStar) << "key " << outKeys[row];
        EXPECT_EQ(countArg[row], ref.CountArg) << "key " << outKeys[row];
        EXPECT_EQ(IsValid(result.Columns[3], row), ref.Seen);
        if (ref.Seen) {
            EXPECT_EQ(sums[row], ref.Sum) << "key " << outKeys[row];
            EXPECT_EQ(mins[row], ref.Min) << "key " << outKeys[row];
            EXPECT_EQ(maxs[row], ref.Max) << "key " << outKeys[row];
        }
    }
    Release(&result);
}

// Regression: a non-nullable argument column must keep aggregate outputs
// non-nullable (no mask), and count(arg) must equal count(*) since no argument
// can be skipped.
TEST(AggregateE2E, NonNullableReducerArgumentKeepsAggregateOutputNonNull) {
    std::array<int64_t, 5> keys = {1, 1, 2, 2, 2};
    std::array<int64_t, 5> values = {10, 4, 7, 3, 5};
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(keys.data())},
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 5,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(),
         std::make_shared<TIntegerType>()});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg cn count v) (agg s sum v) "
        "(agg mn min v) (agg mx max v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 2);
    // Non-nullable argument: no aggregate output carries a mask.
    for (int col = 1; col <= 5; ++col) {
        EXPECT_EQ(result.Columns[col].Mask, nullptr) << "column " << col;
    }
    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* countStar = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* countArg = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* mins = reinterpret_cast<int64_t*>(result.Columns[4].Data);
    auto* maxs = reinterpret_cast<int64_t*>(result.Columns[5].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        EXPECT_EQ(countStar[row], countArg[row]);
        if (outKeys[row] == 1) {
            EXPECT_EQ(countStar[row], 2);
            EXPECT_EQ(sums[row], 14);
            EXPECT_EQ(mins[row], 4);
            EXPECT_EQ(maxs[row], 10);
        } else {
            ASSERT_EQ(outKeys[row], 2);
            EXPECT_EQ(countStar[row], 3);
            EXPECT_EQ(sums[row], 15);
            EXPECT_EQ(mins[row], 3);
            EXPECT_EQ(maxs[row], 7);
        }
    }
    Release(&result);
}

// Composite {i32, string} key combined with a nullable argument across all four
// aggregate functions.
TEST(AggregateE2E, NullableReducerArgumentWithCompositeStringKey) {
    std::array<int32_t, 5> ids = {1, 1, 2, 2, 1};
    std::string names = "aabba";
    std::array<int32_t, 6> offsets = {0, 1, 2, 3, 4, 5};
    std::array<int64_t, 5> values = {10, 999, 4, 6, 888};
    std::array<uint8_t, 1> mask = {0b00001101}; // valid at rows 0, 2, 3
    std::vector<TColumn> columns = {
        TColumn{.Data = reinterpret_cast<char*>(ids.data())},
        TColumn{.Data = names.data(), .Mask = nullptr,
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values.data()),
            .Mask = mask.data()},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 3, .RowCount = 5,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"id", "name", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(TIntegerType::I32),
         std::make_shared<TStringType>(),
         std::make_shared<TNullable>(std::make_shared<TIntegerType>())});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys id name) "
        "(agg c count) (agg cn count v) (agg s sum v) "
        "(agg mn min v) (agg mx max v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 7); // id, name, c, cn, s, mn, mx
    ASSERT_EQ(result.RowCount, 2);
    auto* outIds = reinterpret_cast<int32_t*>(result.Columns[0].Data);
    auto* outOffsets = static_cast<int64_t*>(result.Columns[1].Offsets);
    auto* countStar = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* countArg = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[4].Data);
    auto* mins = reinterpret_cast<int64_t*>(result.Columns[5].Data);
    auto* maxs = reinterpret_cast<int64_t*>(result.Columns[6].Data);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        const std::string name(result.Columns[1].Data + outOffsets[row],
            result.Columns[1].Data + outOffsets[row + 1]);
        if (outIds[row] == 1) {
            EXPECT_EQ(name, "a");
            EXPECT_EQ(countStar[row], 3);
            EXPECT_EQ(countArg[row], 1);
            EXPECT_EQ(sums[row], 10);
            EXPECT_EQ(mins[row], 10);
            EXPECT_EQ(maxs[row], 10);
        } else {
            ASSERT_EQ(outIds[row], 2);
            EXPECT_EQ(name, "b");
            EXPECT_EQ(countStar[row], 2);
            EXPECT_EQ(countArg[row], 2);
            EXPECT_EQ(sums[row], 10);
            EXPECT_EQ(mins[row], 4);
            EXPECT_EQ(maxs[row], 6);
        }
        EXPECT_TRUE(IsValid(result.Columns[4], row));
        EXPECT_TRUE(IsValid(result.Columns[5], row));
        EXPECT_TRUE(IsValid(result.Columns[6], row));
    }
    Release(&result);
}

// Null string key combined with a nullable argument: the NULL-key group and the
// valid-key group must each track argument validity independently.
TEST(AggregateE2E, NullableReducerArgumentWithNullStringKey) {
    std::string data = "xx"; // rows: "x", "", "", "x"
    std::array<int32_t, 5> offsets = {0, 1, 1, 1, 2};
    std::array<uint8_t, 1> keyMask = {0b00001001}; // key valid at rows 0, 3
    std::array<int64_t, 4> values = {10, 999, 5, 888};
    std::array<uint8_t, 1> valMask = {0b00000101}; // arg valid at rows 0, 2
    std::vector<TColumn> columns = {
        TColumn{.Data = data.data(), .Mask = keyMask.data(),
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = reinterpret_cast<char*>(values.data()),
            .Mask = valMask.data()},
    };
    std::vector<TRowSet> batches = {TRowSet{
        .Columns = columns.data(), .ColumnCount = 2, .RowCount = 4,
        .Selection = nullptr, .RefCount = 1}};
    NQdb::TMockSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TNullable>(std::make_shared<TStringType>()),
         std::make_shared<TNullable>(std::make_shared<TIntegerType>())});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg cn count v) (agg s sum v))",
        source);
    auto runtime = RunPlan(root);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.RowCount, 2);
    auto* countStar = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* countArg = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* sums = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* outOffsets = static_cast<int64_t*>(result.Columns[0].Offsets);
    for (int64_t row = 0; row < result.RowCount; ++row) {
        const std::string key(result.Columns[0].Data + outOffsets[row],
            result.Columns[0].Data + outOffsets[row + 1]);
        // Valid string-key group: rows 0 ("x", v=10) and 3 (v NULL).
        // NULL-key group: rows 1 (v NULL) and 2 (v=5); finalizes to empty bytes.
        EXPECT_EQ(countStar[row], 2);
        EXPECT_EQ(countArg[row], 1);
        if (IsValid(result.Columns[0], row)) {
            EXPECT_EQ(key, "x");
            EXPECT_EQ(sums[row], 10);
        } else {
            EXPECT_TRUE(key.empty());
            EXPECT_EQ(sums[row], 5);
        }
        EXPECT_TRUE(IsValid(result.Columns[3], row));
    }
    Release(&result);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
