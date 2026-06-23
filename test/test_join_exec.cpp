#include <gtest/gtest.h>

#include <qdb/exec/join_exec.h>
#include <qdb/io/io.h>

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace NQdb;
using namespace NQumir::NAst;

namespace {

// Builds a single-i64-column TRowSet over caller-owned data, with a Destroy
// that records being called (to verify Release happens exactly once).
TRowSet MakeI64Batch(std::vector<TColumn>& cols, int64_t* data, int64_t rows,
    int* destroyedFlag) {
    cols.clear();
    cols.push_back(TColumn{.Data = reinterpret_cast<char*>(data)});
    TRowSet rs{
        .Columns = cols.data(),
        .ColumnCount = 1,
        .RowCount = rows,
        .Selection = nullptr,
        .Destroy = nullptr,
        .Private = destroyedFlag,
        .RefCount = 1,
    };
    if (destroyedFlag) {
        rs.Destroy = [](TRowSet* rs) {
            ++*static_cast<int*>(rs->Private);
        };
    }
    return rs;
}

} // namespace

TEST(RowId, PackRoundTrip) {
    EXPECT_EQ(BatchIndex(MakeRowId(0, 0)), 0);
    EXPECT_EQ(RowIndex(MakeRowId(0, 0)), 0);
    EXPECT_EQ(BatchIndex(MakeRowId(3, 7)), 3);
    EXPECT_EQ(RowIndex(MakeRowId(3, 7)), 7);
    EXPECT_EQ(BatchIndex(MakeRowId(12345, 67890)), 12345);
    EXPECT_EQ(RowIndex(MakeRowId(12345, 67890)), 67890);
    // Large rowIdx near 2^31 survives the unsigned round-trip.
    EXPECT_EQ(RowIndex(MakeRowId(1, 2000000000)), 2000000000);
}

TEST(RowStore, ReadsColumnsByBatchAndRowId) {
    std::array<int64_t, 3> a = {10, 20, 30};
    std::array<int64_t, 2> b = {40, 50};
    std::vector<TColumn> colsA, colsB;

    TRowStore store;
    int32_t ia = store.PushBatch(MakeI64Batch(colsA, a.data(), 3, nullptr));
    int32_t ib = store.PushBatch(MakeI64Batch(colsB, b.data(), 2, nullptr));

    EXPECT_EQ(store.BatchCount(), 2);
    EXPECT_EQ(ia, 0);
    EXPECT_EQ(ib, 1);

    auto valAt = [&](TRowId id) {
        const TColumn& col = store.Column(id, 0);
        return reinterpret_cast<const int64_t*>(col.Data)[RowIndex(id)];
    };
    EXPECT_EQ(valAt(MakeRowId(ia, 0)), 10);
    EXPECT_EQ(valAt(MakeRowId(ia, 2)), 30);
    EXPECT_EQ(valAt(MakeRowId(ib, 1)), 50);

    // Column(batchIdx, colIdx) overload.
    EXPECT_EQ(reinterpret_cast<const int64_t*>(store.Column(ia, 0).Data)[1], 20);
}

TEST(RowStore, ReleasesEachBatchExactlyOnceOnDestruction) {
    std::array<int64_t, 1> a = {1};
    std::array<int64_t, 1> b = {2};
    std::vector<TColumn> colsA, colsB;
    int destroyedA = 0;
    int destroyedB = 0;

    {
        TRowStore store;
        store.PushBatch(MakeI64Batch(colsA, a.data(), 1, &destroyedA));
        store.PushBatch(MakeI64Batch(colsB, b.data(), 1, &destroyedB));
        EXPECT_EQ(destroyedA, 0);
        EXPECT_EQ(destroyedB, 0);
    }
    // Destructor Released both (RefCount 1 -> 0 -> Destroy) exactly once.
    EXPECT_EQ(destroyedA, 1);
    EXPECT_EQ(destroyedB, 1);
}

namespace {

bool OutValid(const TColumn& col, size_t j) {
    if (!col.Mask) {
        return true;
    }
    return ((col.Mask[j / 8] >> (j % 8)) & 1) != 0;
}

std::string OutString(const TColumn& col, size_t j) {
    const auto* offs = static_cast<const int64_t*>(col.Offsets);
    return std::string(col.Data + offs[j], col.Data + offs[j + 1]);
}

} // namespace

TEST(TakeColumn, FixedI64WithNullPadding) {
    std::array<int64_t, 3> a = {100, 200, 300};
    std::array<int64_t, 2> b = {400, 500};
    std::vector<TColumn> colsA = {TColumn{.Data = reinterpret_cast<char*>(a.data())}};
    std::vector<TColumn> colsB = {TColumn{.Data = reinterpret_cast<char*>(b.data())}};

    TRowStore store;
    int32_t ia = store.PushBatch(TRowSet{
        .Columns = colsA.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1});
    int32_t ib = store.PushBatch(TRowSet{
        .Columns = colsB.data(), .ColumnCount = 1, .RowCount = 2, .RefCount = 1});

    std::vector<TRowId> ids = {
        MakeRowId(ia, 2), MakeRowId(ib, 0), kNullRowId, MakeRowId(ia, 0)};
    TGatheredColumn out;
    TakeColumn(store, ids, 0, std::make_shared<TIntegerType>(TIntegerType::I64), out);

    const auto* v = reinterpret_cast<const int64_t*>(out.Column.Data);
    EXPECT_EQ(v[0], 300);
    EXPECT_EQ(v[1], 400);
    EXPECT_EQ(v[3], 100);
    EXPECT_TRUE(OutValid(out.Column, 0));
    EXPECT_FALSE(OutValid(out.Column, 2)); // kNullRowId -> null
    EXPECT_TRUE(OutValid(out.Column, 3));
}

TEST(TakeColumn, AllValidProducesNoMask) {
    std::array<int64_t, 3> a = {1, 2, 3};
    std::vector<TColumn> cols = {TColumn{.Data = reinterpret_cast<char*>(a.data())}};
    TRowStore store;
    int32_t i = store.PushBatch(TRowSet{
        .Columns = cols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1});

    std::vector<TRowId> ids = {MakeRowId(i, 0), MakeRowId(i, 2)};
    TGatheredColumn out;
    TakeColumn(store, ids, 0, std::make_shared<TIntegerType>(TIntegerType::I64), out);
    EXPECT_EQ(out.Column.Mask, nullptr); // no nulls -> no mask
}

TEST(TakeColumn, HonorsSourceNullMask) {
    std::array<int64_t, 3> a = {7, 8, 9};
    // row 1 is null: bits valid,invalid,valid -> 0b101 = 0x05.
    std::array<uint8_t, 1> mask = {0x05};
    std::vector<TColumn> cols = {TColumn{
        .Data = reinterpret_cast<char*>(a.data()), .Mask = mask.data()}};
    TRowStore store;
    int32_t i = store.PushBatch(TRowSet{
        .Columns = cols.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1});

    std::vector<TRowId> ids = {MakeRowId(i, 0), MakeRowId(i, 1), MakeRowId(i, 2)};
    TGatheredColumn out;
    TakeColumn(store, ids, 0, std::make_shared<TIntegerType>(TIntegerType::I64), out);

    const auto* v = reinterpret_cast<const int64_t*>(out.Column.Data);
    EXPECT_TRUE(OutValid(out.Column, 0));
    EXPECT_EQ(v[0], 7);
    EXPECT_FALSE(OutValid(out.Column, 1)); // source null preserved
    EXPECT_TRUE(OutValid(out.Column, 2));
    EXPECT_EQ(v[2], 9);
}

TEST(TakeColumn, StringGatherWithNullPadding) {
    std::string dataA = "aabbbc";          // "aa","bbb","c"
    std::array<int32_t, 4> offA = {0, 2, 5, 6};
    std::string dataB = "dddd";            // "dddd",""
    std::array<int32_t, 3> offB = {0, 4, 4};
    std::vector<TColumn> colsA = {TColumn{
        .Data = dataA.data(), .Offsets = offA.data(), .OffsetWidth = 4}};
    std::vector<TColumn> colsB = {TColumn{
        .Data = dataB.data(), .Offsets = offB.data(), .OffsetWidth = 4}};

    TRowStore store;
    int32_t ia = store.PushBatch(TRowSet{
        .Columns = colsA.data(), .ColumnCount = 1, .RowCount = 3, .RefCount = 1});
    int32_t ib = store.PushBatch(TRowSet{
        .Columns = colsB.data(), .ColumnCount = 1, .RowCount = 2, .RefCount = 1});

    std::vector<TRowId> ids = {
        MakeRowId(ia, 2), MakeRowId(ib, 0), kNullRowId, MakeRowId(ia, 0), MakeRowId(ib, 1)};
    TGatheredColumn out;
    TakeColumn(store, ids, 0, std::make_shared<TStringType>(), out);

    EXPECT_EQ(out.Column.OffsetWidth, 8);
    EXPECT_EQ(OutString(out.Column, 0), "c");
    EXPECT_EQ(OutString(out.Column, 1), "dddd");
    EXPECT_FALSE(OutValid(out.Column, 2)); // null padding
    EXPECT_EQ(OutString(out.Column, 3), "aa");
    EXPECT_EQ(OutString(out.Column, 4), ""); // empty string stays valid
    EXPECT_TRUE(OutValid(out.Column, 4));
}

namespace {

// One output row materialized back from a TRowSet for comparison.
struct TOutRow {
    int64_t LeftKey;
    int64_t L;
    int64_t R;
    bool operator==(const TOutRow&) const = default;
};

TRowSet MakeKeyPayloadBatch(std::vector<TColumn>& cols,
    int64_t* keys, int64_t* payload, int64_t rows) {
    cols.clear();
    cols.push_back(TColumn{.Data = reinterpret_cast<char*>(keys)});
    cols.push_back(TColumn{.Data = reinterpret_cast<char*>(payload)});
    return TRowSet{.Columns = cols.data(), .ColumnCount = 2, .RowCount = rows,
        .RefCount = 1};
}

} // namespace

TEST(JoinOutputBuilder, StubMatcherMatchesNestedLoopAndChunks) {
    // Left batch: keys [1,2,1], payload l [10,20,30].
    std::array<int64_t, 3> lkeys = {1, 2, 1};
    std::array<int64_t, 3> lpay = {10, 20, 30};
    // Right batch: keys [1,1,3], payload r [100,200,300].
    std::array<int64_t, 3> rkeys = {1, 1, 3};
    std::array<int64_t, 3> rpay = {100, 200, 300};

    std::vector<TColumn> lcols, rcols;
    TRowStore left, right;
    int32_t lb = left.PushBatch(MakeKeyPayloadBatch(lcols, lkeys.data(), lpay.data(), 3));
    int32_t rb = right.PushBatch(MakeKeyPayloadBatch(rcols, rkeys.data(), rpay.data(), 3));

    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    // Output: left key (col0 left), l (col1 left), r (col1 right).
    std::vector<TJoinColumnRef> columns = {
        {EJoinSide::Left, 0, i64},
        {EJoinSide::Left, 1, i64},
        {EJoinSide::Right, 1, i64},
    };

    // Expected via direct nested loop over keys.
    std::vector<TOutRow> expected;
    for (int li = 0; li < 3; ++li) {
        for (int ri = 0; ri < 3; ++ri) {
            if (lkeys[li] == rkeys[ri]) {
                expected.push_back({lkeys[li], lpay[li], rpay[ri]});
            }
        }
    }
    ASSERT_EQ(expected.size(), 4u);

    // batchRows = 2 forces chunking (4 matches -> 2 batches).
    TJoinOutputBuilder builder(&left, &right, columns, /*batchRows=*/2);
    // Stub matcher: nested loop fills pairs.
    for (int li = 0; li < 3; ++li) {
        for (int ri = 0; ri < 3; ++ri) {
            if (lkeys[li] == rkeys[ri]) {
                builder.AddPair(MakeRowId(lb, li), MakeRowId(rb, ri));
            }
        }
    }

    std::vector<TOutRow> got;
    std::vector<int64_t> batchSizes;
    TRowSet out{};
    while (builder.NextBatch(out)) {
        batchSizes.push_back(out.RowCount);
        const auto* k = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* l = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* r = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        for (int64_t j = 0; j < out.RowCount; ++j) {
            got.push_back({k[j], l[j], r[j]});
        }
        Release(&out);
    }

    // Two batches of 2 rows each.
    EXPECT_EQ(batchSizes, (std::vector<int64_t>{2, 2}));

    // Same multiset of rows as the nested-loop oracle.
    auto sortKey = [](const TOutRow& x) { return std::tuple(x.LeftKey, x.L, x.R); };
    std::sort(got.begin(), got.end(),
        [&](auto& a, auto& b) { return sortKey(a) < sortKey(b); });
    std::sort(expected.begin(), expected.end(),
        [&](auto& a, auto& b) { return sortKey(a) < sortKey(b); });
    EXPECT_EQ(got, expected);
}

TEST(JoinOutputBuilder, EmptyPairsProduceNoBatch) {
    TRowStore left, right;
    TJoinOutputBuilder builder(&left, &right, {});
    TRowSet out{};
    EXPECT_FALSE(builder.NextBatch(out));
}

namespace {

using namespace NQumir::NAst;

// IRuntimeNode that yields pre-built batches (Destroy=nullptr, data test-owned).
struct TVectorRuntimeSource : IRuntimeNode {
    TTypePtr Type;
    std::vector<TRowSet> Batches;
    size_t Index = 0;

    TVectorRuntimeSource(TTypePtr type, std::vector<TRowSet> batches)
        : Type(std::move(type)), Batches(std::move(batches)) {}

    TTypePtr OutputType() const override { return Type; }
    bool Next(TRowSet& out) override {
        if (Index >= Batches.size()) return false;
        out = Batches[Index++];
        return true;
    }
};

TTypePtr I64Type() { return std::make_shared<TIntegerType>(TIntegerType::I64); }

TTypePtr KeyValSchema(const std::string& key, const std::string& val) {
    return std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{{key, I64Type()}, {val, I64Type()}});
}

// Builds a (key, value) i64 batch over caller-owned column storage.
TRowSet KeyValBatch(int64_t* keys, int64_t* vals, int64_t rows, std::vector<TColumn>& cols) {
    cols = {TColumn{.Data = reinterpret_cast<char*>(keys)},
            TColumn{.Data = reinterpret_cast<char*>(vals)}};
    return TRowSet{.Columns = cols.data(), .ColumnCount = 2, .RowCount = rows, .RefCount = 1};
}

struct TOut4 {
    int64_t Lk, Lv, Rk, Rv;
    auto operator<=>(const TOut4&) const = default;
};

} // namespace

TEST(RuntimeJoin, InnerJoinEndToEnd) {
    std::vector<int64_t> lk = {1, 2, 1}, lv = {10, 20, 30};
    std::vector<int64_t> rk = {1, 1, 3}, rv = {100, 200, 300};
    std::vector<TColumn> lcols, rcols;

    auto leftType = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {KeyValBatch(lk.data(), lv.data(), 3, lcols)};
    std::vector<TRowSet> rbatches = {KeyValBatch(rk.data(), rv.data(), 3, rcols)};

    auto left = std::make_unique<TVectorRuntimeSource>(leftType, std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::move(rbatches));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::Inner);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::Inner);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType, std::move(kernels), EJoinType::Inner);

    std::vector<TOut4> got;
    TRowSet out{};
    while (join.Next(out)) {
        ASSERT_EQ(out.ColumnCount, 4);
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c1 = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* c2 = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        const auto* c3 = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.push_back({c0[i], c1[i], c2[i], c3[i]});
        }
        Release(&out);
    }

    std::vector<TOut4> expected = {
        {1, 10, 1, 100}, {1, 10, 1, 200}, {1, 30, 1, 100}, {1, 30, 1, 200}};
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(RuntimeJoin, InnerJoinStreamsRightAfterLeftEof) {
    std::vector<int64_t> lk = {1, 2, 1}, lv = {10, 20, 30};
    std::vector<int64_t> rk0 = {2, 3}, rv0 = {200, 300};
    std::vector<int64_t> rk1 = {1, 1}, rv1 = {100, 101};
    std::vector<TColumn> lcols, rcols0, rcols1;

    auto leftType = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {KeyValBatch(lk.data(), lv.data(), 3, lcols)};
    std::vector<TRowSet> rbatches = {
        KeyValBatch(rk0.data(), rv0.data(), 2, rcols0),
        KeyValBatch(rk1.data(), rv1.data(), 2, rcols1),
    };

    auto left = std::make_unique<TVectorRuntimeSource>(leftType, std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::move(rbatches));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::Inner);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::Inner);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType, std::move(kernels), EJoinType::Inner);

    std::vector<TOut4> got;
    TRowSet out{};
    while (join.Next(out)) {
        ASSERT_EQ(out.ColumnCount, 4);
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c1 = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* c2 = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        const auto* c3 = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.push_back({c0[i], c1[i], c2[i], c3[i]});
        }
        Release(&out);
    }

    std::vector<TOut4> expected = {
        {1, 10, 1, 100}, {1, 10, 1, 101},
        {1, 30, 1, 100}, {1, 30, 1, 101},
        {2, 20, 2, 200},
    };
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(RuntimeJoin, InnerJoinStreamsLeftAfterRightEof) {
    std::vector<int64_t> lk0 = {2, 3}, lv0 = {20, 30};
    std::vector<int64_t> lk1 = {1, 1}, lv1 = {10, 11};
    std::vector<int64_t> rk = {1, 2, 1}, rv = {100, 200, 101};
    std::vector<TColumn> lcols0, lcols1, rcols;

    auto leftType = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {
        KeyValBatch(lk0.data(), lv0.data(), 2, lcols0),
        KeyValBatch(lk1.data(), lv1.data(), 2, lcols1),
    };
    std::vector<TRowSet> rbatches = {KeyValBatch(rk.data(), rv.data(), 3, rcols)};

    auto left = std::make_unique<TVectorRuntimeSource>(leftType, std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::move(rbatches));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::Inner);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::Inner);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType, std::move(kernels), EJoinType::Inner);

    std::vector<TOut4> got;
    TRowSet out{};
    while (join.Next(out)) {
        ASSERT_EQ(out.ColumnCount, 4);
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c1 = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* c2 = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        const auto* c3 = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.push_back({c0[i], c1[i], c2[i], c3[i]});
        }
        Release(&out);
    }

    std::vector<TOut4> expected = {
        {1, 10, 1, 100}, {1, 10, 1, 101},
        {1, 11, 1, 100}, {1, 11, 1, 101},
        {2, 20, 2, 200},
    };
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(RuntimeJoin, InnerJoinResidualReadsStreamBatchAfterEof) {
    std::vector<int64_t> lk = {1, 1}, lv = {10, 20};
    std::vector<int64_t> rk0 = {1}, rv0 = {10};
    std::vector<int64_t> rk1 = {1}, rv1 = {20};
    std::vector<TColumn> lcols, rcols0, rcols1;

    auto leftType = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {KeyValBatch(lk.data(), lv.data(), 2, lcols)};
    std::vector<TRowSet> rbatches = {
        KeyValBatch(rk0.data(), rv0.data(), 1, rcols0),
        KeyValBatch(rk1.data(), rv1.data(), 1, rcols1),
    };

    auto left = std::make_unique<TVectorRuntimeSource>(leftType, std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::move(rbatches));

    auto residual = std::make_shared<TBinaryExpr>(
        NQumir::TLocation{}, TOperator("!="),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "lv"),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "rv"));
    TStructType innerType({
        {"lk", I64Type()}, {"lv", I64Type()},
        {"rk", I64Type()}, {"rv", I64Type()},
    });

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::Inner, residual, &innerType, 2);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::Inner);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType,
        std::move(kernels), EJoinType::Inner, /*hasResidual=*/true);

    std::vector<TOut4> got;
    TRowSet out{};
    while (join.Next(out)) {
        ASSERT_EQ(out.ColumnCount, 4);
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c1 = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* c2 = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        const auto* c3 = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.push_back({c0[i], c1[i], c2[i], c3[i]});
        }
        Release(&out);
    }

    std::vector<TOut4> expected = {
        {1, 20, 1, 10},
        {1, 10, 1, 20},
    };
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(RuntimeJoin, MultipleBatchesMatchNestedLoop) {
    int seed = 777;
    auto rnd = [&]() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; };
    std::vector<int64_t> lk, lv, rk, rv;
    for (int i = 0; i < 50; ++i) { lk.push_back(rnd() % 12); lv.push_back(i); }
    for (int i = 0; i < 45; ++i) { rk.push_back(rnd() % 12); rv.push_back(1000 + i); }

    // Split each side into batches of 10 rows to exercise multi-batch RowIds.
    auto leftType = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<std::vector<TColumn>> colStorage;
    auto split = [&](std::vector<int64_t>& keys, std::vector<int64_t>& vals) {
        std::vector<TRowSet> batches;
        for (size_t off = 0; off < keys.size(); off += 10) {
            int64_t n = std::min<int64_t>(10, keys.size() - off);
            colStorage.emplace_back();
            batches.push_back(KeyValBatch(keys.data() + off, vals.data() + off, n, colStorage.back()));
        }
        return batches;
    };
    auto lbatches = split(lk, lv);
    auto rbatches = split(rk, rv);

    auto left = std::make_unique<TVectorRuntimeSource>(leftType, std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::move(rbatches));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::Inner);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::Inner);
    ASSERT_TRUE(outputType);
    TRuntimeJoin join(std::move(left), std::move(right), *outputType, std::move(kernels), EJoinType::Inner);

    int64_t rowCount = 0;
    TRowSet out{};
    while (join.Next(out)) {
        for (int64_t i = 0; i < out.RowCount; ++i) {
            int64_t lkv = reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i];
            int64_t rkv = reinterpret_cast<const int64_t*>(out.Columns[2].Data)[i];
            ASSERT_EQ(lkv, rkv); // every emitted row has matching keys
        }
        rowCount += out.RowCount;
        Release(&out);
    }

    int64_t expected = 0;
    for (int64_t a : lk) for (int64_t b : rk) if (a == b) ++expected;
    EXPECT_EQ(rowCount, expected);
}

namespace {

// Semi/Anti oracle: for each left row, check whether its key exists in the
// right set. Returns the (key, value) pairs of left rows that satisfy the
// SEMI or ANTI condition.
std::vector<std::pair<int64_t,int64_t>> SemiAntiOracle(
    const std::vector<int64_t>& lk, const std::vector<int64_t>& lv,
    const std::vector<int64_t>& rk, bool isAnti)
{
    std::unordered_set<int64_t> rset(rk.begin(), rk.end());
    std::vector<std::pair<int64_t,int64_t>> result;
    for (size_t i = 0; i < lk.size(); ++i) {
        bool found = rset.count(lk[i]) > 0;
        if (found != isAnti) {
            result.emplace_back(lk[i], lv[i]);
        }
    }
    return result;
}

std::vector<std::pair<int64_t,int64_t>> DrainSemiAntiJoin(
    TRuntimeJoin& join, int expectedCols)
{
    std::vector<std::pair<int64_t,int64_t>> got;
    TRowSet out{};
    while (join.Next(out)) {
        EXPECT_EQ(out.ColumnCount, expectedCols);
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c1 = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(c0[i], c1[i]);
        }
        Release(&out);
    }
    return got;
}

} // namespace

#include <unordered_set>

TEST(RuntimeJoin, LeftSemiScalarKey) {
    std::vector<int64_t> lk = {1, 2, 3, 4}, lv = {10, 20, 30, 40};
    std::vector<int64_t> rk = {2, 3, 5},    rv = {200, 300, 500};
    std::vector<TColumn> lcols, rcols;

    auto leftType  = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {KeyValBatch(lk.data(), lv.data(), 4, lcols)};
    std::vector<TRowSet> rbatches = {KeyValBatch(rk.data(), rv.data(), 3, rcols)};

    auto left  = std::make_unique<TVectorRuntimeSource>(leftType,  std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::move(rbatches));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::LeftSemi);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::LeftSemi);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType, std::move(kernels), EJoinType::LeftSemi);
    auto got = DrainSemiAntiJoin(join, /*cols=*/2);

    auto expected = SemiAntiOracle(lk, lv, rk, /*isAnti=*/false);
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected); // {(2,20),(3,30)}
}

TEST(RuntimeJoin, LeftAntiScalarKey) {
    std::vector<int64_t> lk = {1, 2, 3, 4}, lv = {10, 20, 30, 40};
    std::vector<int64_t> rk = {2, 3, 5},    rv = {200, 300, 500};
    std::vector<TColumn> lcols, rcols;

    auto leftType  = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {KeyValBatch(lk.data(), lv.data(), 4, lcols)};
    std::vector<TRowSet> rbatches = {KeyValBatch(rk.data(), rv.data(), 3, rcols)};

    auto left  = std::make_unique<TVectorRuntimeSource>(leftType,  std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::move(rbatches));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::LeftAnti);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::LeftAnti);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType, std::move(kernels), EJoinType::LeftAnti);
    auto got = DrainSemiAntiJoin(join, /*cols=*/2);

    auto expected = SemiAntiOracle(lk, lv, rk, /*isAnti=*/true);
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected); // {(1,10),(4,40)}
}

TEST(RuntimeJoin, LeftSemiResidualStreamsRight) {
    std::vector<int64_t> lk = {1, 1, 2}, lv = {10, 20, 30};
    std::vector<int64_t> rk = {1, 2}, rv = {10, 30};
    std::vector<TColumn> lcols, rcols;

    auto leftType  = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {KeyValBatch(lk.data(), lv.data(), 3, lcols)};
    std::vector<TRowSet> rbatches = {KeyValBatch(rk.data(), rv.data(), 2, rcols)};

    auto left  = std::make_unique<TVectorRuntimeSource>(leftType,  std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::move(rbatches));

    auto residual = std::make_shared<TBinaryExpr>(
        NQumir::TLocation{}, TOperator("!="),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "lv"),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "rv"));
    TStructType innerType({
        {"lk", I64Type()}, {"lv", I64Type()},
        {"rk", I64Type()}, {"rv", I64Type()},
    });

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::Inner, residual, &innerType, 2);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::LeftSemi);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType,
        std::move(kernels), EJoinType::LeftSemi, /*hasResidual=*/true);
    auto got = DrainSemiAntiJoin(join, /*cols=*/2);

    std::vector<std::pair<int64_t,int64_t>> expected = {{1, 20}};
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);
}

TEST(RuntimeJoin, LeftAntiResidualStreamsRight) {
    std::vector<int64_t> lk = {1, 1, 2}, lv = {10, 20, 30};
    std::vector<int64_t> rk = {1, 2}, rv = {10, 30};
    std::vector<TColumn> lcols, rcols;

    auto leftType  = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {KeyValBatch(lk.data(), lv.data(), 3, lcols)};
    std::vector<TRowSet> rbatches = {KeyValBatch(rk.data(), rv.data(), 2, rcols)};

    auto left  = std::make_unique<TVectorRuntimeSource>(leftType,  std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::move(rbatches));

    auto residual = std::make_shared<TBinaryExpr>(
        NQumir::TLocation{}, TOperator("!="),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "lv"),
        std::make_shared<TIdentExpr>(NQumir::TLocation{}, "rv"));
    TStructType innerType({
        {"lk", I64Type()}, {"lv", I64Type()},
        {"rk", I64Type()}, {"rv", I64Type()},
    });

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::Inner, residual, &innerType, 2);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::LeftAnti);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType,
        std::move(kernels), EJoinType::LeftAnti, /*hasResidual=*/true);
    auto got = DrainSemiAntiJoin(join, /*cols=*/2);

    std::vector<std::pair<int64_t,int64_t>> expected = {{1, 10}, {2, 30}};
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(RuntimeJoin, LeftSemiEmptyRight) {
    std::vector<int64_t> lk = {1, 2, 3}, lv = {10, 20, 30};
    std::vector<TColumn> lcols;

    auto leftType  = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {KeyValBatch(lk.data(), lv.data(), 3, lcols)};

    auto left  = std::make_unique<TVectorRuntimeSource>(leftType,  std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::vector<TRowSet>{});

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::LeftSemi);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::LeftSemi);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType, std::move(kernels), EJoinType::LeftSemi);
    auto got = DrainSemiAntiJoin(join, /*cols=*/2);
    EXPECT_TRUE(got.empty()); // no right rows → no matches
}

TEST(RuntimeJoin, LeftAntiEmptyRight) {
    std::vector<int64_t> lk = {1, 2, 3}, lv = {10, 20, 30};
    std::vector<TColumn> lcols;

    auto leftType  = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {KeyValBatch(lk.data(), lv.data(), 3, lcols)};

    auto left  = std::make_unique<TVectorRuntimeSource>(leftType,  std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::vector<TRowSet>{});

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::LeftAnti);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::LeftAnti);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType, std::move(kernels), EJoinType::LeftAnti);
    auto got = DrainSemiAntiJoin(join, /*cols=*/2);

    // All left rows pass when right is empty.
    std::vector<std::pair<int64_t,int64_t>> expected = {{1,10},{2,20},{3,30}};
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, expected);
}

TEST(RuntimeJoin, LeftSemiMultiBatch) {
    int seed = 42;
    auto rnd = [&]() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; };
    std::vector<int64_t> lk, lv, rk, rv;
    for (int i = 0; i < 60; ++i) { lk.push_back(rnd() % 15); lv.push_back(i); }
    for (int i = 0; i < 40; ++i) { rk.push_back(rnd() % 15); rv.push_back(1000 + i); }

    auto leftType  = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<std::vector<TColumn>> colStorage;
    auto split = [&](std::vector<int64_t>& keys, std::vector<int64_t>& vals) {
        std::vector<TRowSet> batches;
        for (size_t off = 0; off < keys.size(); off += 10) {
            int64_t n = std::min<int64_t>(10, (int64_t)keys.size() - (int64_t)off);
            colStorage.emplace_back();
            batches.push_back(KeyValBatch(keys.data() + off, vals.data() + off, n, colStorage.back()));
        }
        return batches;
    };

    auto left  = std::make_unique<TVectorRuntimeSource>(leftType,  split(lk, lv));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, split(rk, rv));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::LeftSemi);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::LeftSemi);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType, std::move(kernels), EJoinType::LeftSemi);
    auto got = DrainSemiAntiJoin(join, /*cols=*/2);

    auto expected = SemiAntiOracle(lk, lv, rk, /*isAnti=*/false);
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(RuntimeJoin, LeftAntiMultiBatch) {
    int seed = 99;
    auto rnd = [&]() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; };
    std::vector<int64_t> lk, lv, rk, rv;
    for (int i = 0; i < 55; ++i) { lk.push_back(rnd() % 10); lv.push_back(i); }
    for (int i = 0; i < 30; ++i) { rk.push_back(rnd() % 10); rv.push_back(2000 + i); }

    auto leftType  = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<std::vector<TColumn>> colStorage;
    auto split = [&](std::vector<int64_t>& keys, std::vector<int64_t>& vals) {
        std::vector<TRowSet> batches;
        for (size_t off = 0; off < keys.size(); off += 10) {
            int64_t n = std::min<int64_t>(10, (int64_t)keys.size() - (int64_t)off);
            colStorage.emplace_back();
            batches.push_back(KeyValBatch(keys.data() + off, vals.data() + off, n, colStorage.back()));
        }
        return batches;
    };

    auto left  = std::make_unique<TVectorRuntimeSource>(leftType,  split(lk, lv));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, split(rk, rv));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::LeftAnti);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::LeftAnti);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType, std::move(kernels), EJoinType::LeftAnti);
    auto got = DrainSemiAntiJoin(join, /*cols=*/2);

    auto expected = SemiAntiOracle(lk, lv, rk, /*isAnti=*/true);
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

// ─── Left / Right Outer Join ───────────────────────────────────────────────

namespace {

// Left outer oracle: for each left row (lk[i], lv[i]), find all right rows with
// matching key. If none found, emit (lk[i], lv[i], nullKey, nullVal) where null
// values are represented as -1 (kNullRowId rows → mask bit 0 → zeroed data, but
// we use -1 sentinel for comparison in the oracle).
struct TOuterRow {
    int64_t Lk, Lv;
    bool RNull;   // true  → right side is NULL
    int64_t Rk, Rv;
    auto operator<=>(const TOuterRow&) const = default;
};

std::vector<TOuterRow> LeftOuterOracle(
    const std::vector<int64_t>& lk, const std::vector<int64_t>& lv,
    const std::vector<int64_t>& rk, const std::vector<int64_t>& rv)
{
    std::vector<TOuterRow> result;
    for (size_t i = 0; i < lk.size(); ++i) {
        bool matched = false;
        for (size_t j = 0; j < rk.size(); ++j) {
            if (lk[i] == rk[j]) {
                result.push_back({lk[i], lv[i], false, rk[j], rv[j]});
                matched = true;
            }
        }
        if (!matched) {
            result.push_back({lk[i], lv[i], true, 0, 0});
        }
    }
    return result;
}

// Drains a Left outer join — expects 4 columns (lk, lv, rk, rv).
// Right-side nulls are detected via the mask and represented as (rNull=true, rk=0, rv=0).
std::vector<TOuterRow> DrainOuterJoin(TRuntimeJoin& join) {
    std::vector<TOuterRow> got;
    TRowSet out{};
    while (join.Next(out)) {
        EXPECT_EQ(out.ColumnCount, 4);
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c1 = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* c2 = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        const auto* c3 = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            bool rNull = !OutValid(out.Columns[2], static_cast<size_t>(i));
            got.push_back({c0[i], c1[i], rNull,
                rNull ? int64_t{0} : c2[i],
                rNull ? int64_t{0} : c3[i]});
        }
        Release(&out);
    }
    return got;
}

} // namespace

TEST(RuntimeJoin, LeftOuterScalarKey) {
    std::vector<int64_t> lk = {1, 2, 3, 4}, lv = {10, 20, 30, 40};
    std::vector<int64_t> rk = {2, 3, 5},    rv = {200, 300, 500};
    std::vector<TColumn> lcols, rcols;

    auto leftType  = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {KeyValBatch(lk.data(), lv.data(), 4, lcols)};
    std::vector<TRowSet> rbatches = {KeyValBatch(rk.data(), rv.data(), 3, rcols)};

    auto left  = std::make_unique<TVectorRuntimeSource>(leftType,  std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::move(rbatches));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::Left);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::Left);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType,
        std::move(kernels), EJoinType::Left);
    auto got = DrainOuterJoin(join);

    auto expected = LeftOuterOracle(lk, lv, rk, rv);
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(RuntimeJoin, LeftOuterEmptyRight) {
    std::vector<int64_t> lk = {1, 2, 3}, lv = {10, 20, 30};
    std::vector<int64_t> rk = {},         rv = {};
    std::vector<TColumn> lcols, rcols;

    auto leftType  = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {KeyValBatch(lk.data(), lv.data(), 3, lcols)};
    std::vector<TRowSet> rbatches = {};

    auto left  = std::make_unique<TVectorRuntimeSource>(leftType,  std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::move(rbatches));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::Left);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::Left);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType,
        std::move(kernels), EJoinType::Left);
    auto got = DrainOuterJoin(join);

    auto expected = LeftOuterOracle(lk, lv, rk, rv);
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected); // all left rows, rNull=true
}

TEST(RuntimeJoin, LeftOuterMultiBatch) {
    int seed = 42;
    auto rnd = [&]() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; };
    std::vector<int64_t> lk, lv, rk, rv;
    for (int i = 0; i < 30; ++i) { lk.push_back(rnd() % 8); lv.push_back(i); }
    for (int i = 0; i < 25; ++i) { rk.push_back(rnd() % 8); rv.push_back(100 + i); }

    auto leftType  = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<std::vector<TColumn>> colStorage;
    auto split = [&](std::vector<int64_t>& keys, std::vector<int64_t>& vals) {
        std::vector<TRowSet> batches;
        for (size_t off = 0; off < keys.size(); off += 10) {
            int64_t n = std::min<int64_t>(10, static_cast<int64_t>(keys.size() - off));
            colStorage.emplace_back();
            batches.push_back(KeyValBatch(keys.data() + off, vals.data() + off, n, colStorage.back()));
        }
        return batches;
    };

    auto left  = std::make_unique<TVectorRuntimeSource>(leftType,  split(lk, lv));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, split(rk, rv));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::Left);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::Left);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType,
        std::move(kernels), EJoinType::Left);
    auto got = DrainOuterJoin(join);

    auto expected = LeftOuterOracle(lk, lv, rk, rv);
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

// Right outer: equivalent to Left outer with sides swapped.
// Oracle: for each right row, find all matching left rows; if none, right row has NULL left.
TEST(RuntimeJoin, RightOuterScalarKey) {
    std::vector<int64_t> lk = {2, 3, 5}, lv = {20, 30, 50};
    std::vector<int64_t> rk = {1, 2, 3, 4}, rv = {100, 200, 300, 400};
    std::vector<TColumn> lcols, rcols;

    auto leftType  = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    std::vector<TRowSet> lbatches = {KeyValBatch(lk.data(), lv.data(), 3, lcols)};
    std::vector<TRowSet> rbatches = {KeyValBatch(rk.data(), rv.data(), 4, rcols)};

    auto left  = std::make_unique<TVectorRuntimeSource>(leftType,  std::move(lbatches));
    auto right = std::make_unique<TVectorRuntimeSource>(rightType, std::move(rbatches));

    TKernelCompiler compiler;
    auto kernels = compiler.CompileJoin(
        static_cast<TStructType&>(*leftType), static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, EJoinType::Right);
    auto outputType = ComputeJoinOutputType(leftType, rightType, EJoinType::Right);
    ASSERT_TRUE(outputType);

    TRuntimeJoin join(std::move(left), std::move(right), *outputType,
        std::move(kernels), EJoinType::Right);

    // Drain: for Right join output is [nullable(left) ++ right] = [lk,lv,rk,rv]
    // where left cols are nullable.
    struct TRightRow {
        bool LNull; int64_t Lk, Lv, Rk, Rv;
        auto operator<=>(const TRightRow&) const = default;
    };
    std::vector<TRightRow> got;
    TRowSet out{};
    while (join.Next(out)) {
        EXPECT_EQ(out.ColumnCount, 4);
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c1 = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* c2 = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        const auto* c3 = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            bool lNull = !OutValid(out.Columns[0], static_cast<size_t>(i));
            got.push_back({lNull,
                lNull ? int64_t{0} : c0[i],
                lNull ? int64_t{0} : c1[i],
                c2[i], c3[i]});
        }
        Release(&out);
    }

    // Oracle: for each right row, find matching left rows; unmatched → lNull
    std::vector<TRightRow> expected;
    for (size_t j = 0; j < rk.size(); ++j) {
        bool matched = false;
        for (size_t i = 0; i < lk.size(); ++i) {
            if (lk[i] == rk[j]) {
                expected.push_back({false, lk[i], lv[i], rk[j], rv[j]});
                matched = true;
            }
        }
        if (!matched) {
            expected.push_back({true, 0, 0, rk[j], rv[j]});
        }
    }
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
