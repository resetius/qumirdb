#include <gtest/gtest.h>

#include <qdb/exec/join_exec.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/io/io.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/stats.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace NQdb;
using namespace NQumir::NAst;

namespace {

TTypePtr I64Type() { return std::make_shared<TIntegerType>(TIntegerType::I64); }

TTypePtr KeyValSchema(const std::string& key, const std::string& val) {
    return std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{{key, I64Type()}, {val, I64Type()}});
}

TRowSet KeyValBatch(int64_t* keys, int64_t* vals, int64_t rows, std::vector<TColumn>& cols) {
    cols = {TColumn{.Data = reinterpret_cast<char*>(keys)},
            TColumn{.Data = reinterpret_cast<char*>(vals)}};
    return TRowSet{.Columns = cols.data(), .ColumnCount = 2, .RowCount = rows, .RefCount = 1};
}

struct TOut4 {
    int64_t Lk, Lv, Rk, Rv;
    auto operator<=>(const TOut4&) const = default;
};

TJoinKernels CompileJoin(TKernelCompiler& compiler,
    const TTypePtr& leftType, const TTypePtr& rightType, EJoinType type)
{
    auto spec = NKernel::BuildJoinKernelSpec(
        static_cast<TStructType&>(*leftType),
        static_cast<TStructType&>(*rightType),
        {{"lk", "rk"}}, type, nullptr);
    return compiler.CompileJoin(spec);
}

// Drives a processor with a one-batch build side and a two-batch probe side.
// Asserts the probe is never fetched while RequiredInputSide reports the build
// side, then returns the collected inner-join rows.
std::vector<TOut4> RunAsymmetric(EJoinBuildSide buildSide) {
    std::vector<int64_t> lk = {1, 2, 1}, lv = {10, 20, 30};
    std::vector<int64_t> rk0 = {1, 1}, rv0 = {100, 200};
    std::vector<int64_t> rk1 = {3}, rv1 = {300};
    std::vector<TColumn> lcols, rcols0, rcols1;

    auto leftType = KeyValSchema("lk", "lv");
    auto rightType = KeyValSchema("rk", "rv");
    auto leftBatch = KeyValBatch(lk.data(), lv.data(), 3, lcols);
    auto rightBatch0 = KeyValBatch(rk0.data(), rv0.data(), 2, rcols0);
    auto rightBatch1 = KeyValBatch(rk1.data(), rv1.data(), 1, rcols1);

    TKernelCompiler compiler;
    auto kernels = CompileJoin(compiler, leftType, rightType, EJoinType::Inner);
    TInnerJoinProcessor processor(std::move(kernels), EJoinType::Inner, buildSide);

    // Build side = single batch; probe side = two batches. Which physical input
    // is which depends on buildSide; the probe is the opposite of the build.
    const bool buildIsRight = buildSide == EJoinBuildSide::Right;
    int rightFetches = 0;
    int leftFetches = 0;
    int rightIndex = 0;
    int leftIndex = 0;

    auto right = [&](TRowSet& rowSet) {
        ++rightFetches;
        if (rightIndex == 0) { rowSet = rightBatch0; ++rightIndex; return EJoinFetchResult::OK; }
        if (rightIndex == 1) { rowSet = rightBatch1; ++rightIndex; return EJoinFetchResult::OK; }
        return EJoinFetchResult::FINISHED;
    };
    auto left = [&](TRowSet& rowSet) {
        ++leftFetches;
        if (leftIndex == 0) { rowSet = leftBatch; ++leftIndex; return EJoinFetchResult::OK; }
        return EJoinFetchResult::FINISHED;
    };

    std::vector<TOut4> got;
    TRowSet out{};
    for (;;) {
        // Before each step, the side not yet requested must not have been pulled.
        switch (processor.RequiredInputSide()) {
            case EJoinBuildSide::Left:
                if (!buildIsRight) EXPECT_EQ(rightFetches, 0) << "probe pulled while building";
                break;
            case EJoinBuildSide::Right:
                if (buildIsRight) EXPECT_EQ(leftFetches, 0) << "probe pulled while building";
                break;
            case EJoinBuildSide::Auto:
                break;
        }
        auto result = processor.Process(left, right, out);
        if (result == EJoinProcessorResult::NEED_DATA) continue;
        if (result == EJoinProcessorResult::FINISHED) break;
        const auto* c0 = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* c1 = reinterpret_cast<const int64_t*>(out.Columns[1].Data);
        const auto* c2 = reinterpret_cast<const int64_t*>(out.Columns[2].Data);
        const auto* c3 = reinterpret_cast<const int64_t*>(out.Columns[3].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.push_back({c0[i], c1[i], c2[i], c3[i]});
        }
        Release(&out);
    }
    std::sort(got.begin(), got.end());
    return got;
}

std::vector<TOut4> ExpectedInner() {
    // lk {1,2,1} x rk {1,1,3}: key 1 matches (l rows 10,30) x (r rows 100,200).
    std::vector<TOut4> e = {
        {1, 10, 1, 100}, {1, 10, 1, 200}, {1, 30, 1, 100}, {1, 30, 1, 200}};
    std::sort(e.begin(), e.end());
    return e;
}

// Minimal operator carrying a schema and Stats_, to exercise ChooseJoinBuildSide.
class TFakeSource : public IOperator {
public:
    static constexpr const char* OpId = "fake_source";
    explicit TFakeSource(std::vector<std::pair<std::string, TTypePtr>> fields) {
        auto schema = std::make_shared<TStructType>(std::move(fields));
        Type = std::make_shared<TFunctionType>(std::vector<TTypePtr>{}, schema);
    }
    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override { return {}; }
    std::vector<TExprPtr> Children() const override { return {}; }
    const std::string ToString() const override { return "(rel fake_source)"; }
};

TStatsPtr Rows(uint64_t n) {
    auto s = std::make_shared<TStats>();
    s->RowCount = n;
    return s;
}

TOperatorPtr InnerJoin() {
    auto left = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{{"lk", I64Type()}, {"lv", I64Type()}});
    auto right = std::make_shared<TFakeSource>(
        std::vector<std::pair<std::string, TTypePtr>>{{"rk", I64Type()}, {"rv", I64Type()}});
    auto join = MakeJoin(left, right, {{"lk", "rk"}}, EJoinType::Inner);
    EXPECT_TRUE(join.has_value()) << (join ? "" : join.error().ToString());
    return std::static_pointer_cast<IOperator>(join.value_or(nullptr));
}

} // namespace

TEST(AsymmetricJoin, BuildRightMatchesSymmetric) {
    EXPECT_EQ(RunAsymmetric(EJoinBuildSide::Right), ExpectedInner());
}

TEST(AsymmetricJoin, BuildLeftMatchesSymmetric) {
    EXPECT_EQ(RunAsymmetric(EJoinBuildSide::Left), ExpectedInner());
}

// Decision reads JoinAsymmetryRatio so it tracks the constant rather than a
// hardcoded ratio: size the larger side just past the threshold.
TEST(ChooseJoinBuildSide, PicksSmallerSideWhenRatioMet) {
    const uint64_t small = 1000;
    const uint64_t big = static_cast<uint64_t>(std::ceil(small * JoinAsymmetryRatio)) + 1;

    auto join = InnerJoin();
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());

    j->Left()->Stats_ = Rows(big);
    j->Right()->Stats_ = Rows(small);
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Right);

    j->Left()->Stats_ = Rows(small);
    j->Right()->Stats_ = Rows(big);
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Left);
}

TEST(ChooseJoinBuildSide, AutoWhenBalancedOrStatsMissing) {
    auto join = InnerJoin();
    ASSERT_TRUE(join);
    auto* j = static_cast<TJoinOperator*>(join.get());

    // Equal sizes: ratio (>1) is never met.
    j->Left()->Stats_ = Rows(1000);
    j->Right()->Stats_ = Rows(1000);
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Auto);

    // Missing stats fall back to the adaptive symmetric path.
    j->Right()->Stats_ = nullptr;
    EXPECT_EQ(ChooseJoinBuildSide(*j), EJoinBuildSide::Auto);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
