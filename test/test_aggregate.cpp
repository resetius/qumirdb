#include <gtest/gtest.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include <qdb/exec/executor.h>
#include <qdb/exec/planner.h>
#include <qdb/io/io.h>
#include <qdb/ops/operator.h>
#include <qdb/ops/source.h>
#include <qdb/pipeline/column_pruning.h>
#include <qdb/pipeline/typing.h>
#include <qdb/sexp/parser.h>

#include <algorithm>
#include <memory>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace NQqb;
using namespace NQqb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {

// In-memory ISource over pre-built TRowSet batches.
struct TVectorSource : ISource {
    std::vector<std::string> Names;
    std::vector<TColumnSchema> Cols;
    TSchema Schema_;
    std::vector<TRowSet> Batches;
    size_t Index = 0;

    TVectorSource(
        std::vector<std::string> names,
        std::vector<TRowSet> batches,
        std::vector<TTypePtr> types = {})
        : Names(std::move(names))
        , Batches(std::move(batches))
    {
        if (types.empty()) {
            types.resize(Names.size(),
                std::make_shared<TIntegerType>(TIntegerType::I64));
        }
        if (types.size() != Names.size()) {
            throw std::invalid_argument("TVectorSource names/types size mismatch");
        }
        for (size_t i = 0; i < Names.size(); ++i) {
            Cols.push_back({Names[i], std::move(types[i])});
        }
        Schema_ = TSchema{Cols};
    }

    const TSchema& Schema() const override { return Schema_; }

    bool Next(TRowSet& rowSet) override {
        if (Index >= Batches.size()) {
            return false;
        }
        rowSet = Batches[Index++];
        return true;
    }
};

// Parses `sexp` as a (rel aggregate ...) plan whose (rel source "...") leaf is
// backed by `source`, runs AnnotateTypes + ApplyColumnPruning, and returns the
// resulting logical plan root.
TOperatorPtr ParsePlan(const std::string& sexp, ISource& source) {
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(source, std::string(path));
    };

    TParser parser;
    for (auto& [name, fn] : MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[name] = std::move(fn);
    }

    std::istringstream in(sexp);
    TTokenStream ts(in);
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

    TVectorSource source({"k", "v"}, std::move(batches));
    auto root = ParsePlan(kPlanSexp, source);

    TPhysicalPlanner planner;
    auto runtime = planner.Build(root);

    auto reference = ComputeReference(keys, vals);

    TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, 5); // k, c, s, mn, mx
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));

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

    TVectorSource source({"k", "v"}, std::move(batches));
    auto root = ParsePlan(kPlanSexp, source);

    TPhysicalPlanner planner;
    auto runtime = planner.Build(root);

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

    TVectorSource source({"k1", "k2", "v"}, std::move(batches));
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k1 k2) "
        "(agg c count) (agg s sum v))",
        source);
    TPhysicalPlanner planner;
    auto runtime = planner.Build(root);

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
    TVectorSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(TIntegerType::I32),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg s sum v))",
        source);
    TPhysicalPlanner planner;
    auto runtime = planner.Build(root);

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
    TVectorSource source(
        {"k", "v"}, std::move(batches),
        {std::make_shared<TFloatType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k) "
        "(agg c count) (agg s sum v))",
        source);
    TPhysicalPlanner planner;
    auto runtime = planner.Build(root);

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
    TVectorSource source(
        {"k1", "k2", "v"}, std::move(batches),
        {std::make_shared<TIntegerType>(TIntegerType::I32),
         std::make_shared<TFloatType>(),
         std::make_shared<TIntegerType>(TIntegerType::I64)});
    auto root = ParsePlan(
        "(rel aggregate (rel source \"data.parquet\") (keys k1 k2) "
        "(agg c count) (agg s sum v))",
        source);
    TPhysicalPlanner planner;
    auto runtime = planner.Build(root);

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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
