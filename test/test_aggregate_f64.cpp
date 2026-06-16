#include <gtest/gtest.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include <qdb/exec/executor.h>
#include <qdb/exec/planner.h>
#include <qdb/io/io.h>
#include <qdb/ops/source.h>
#include <qdb/pipeline/column_pruning.h>
#include <qdb/pipeline/typing.h>
#include <qdb/sexp/parser.h>

#include <array>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace NQqb;
using namespace NQqb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {

struct TStubSource : ISource {
    std::vector<std::string> Names;
    std::vector<TColumnSchema> Cols;
    TSchema Schema_;
    std::vector<TRowSet> Batches;
    size_t Index = 0;

    TStubSource(std::vector<std::string> names, std::vector<TTypePtr> types,
        std::vector<TRowSet> batches)
        : Names(std::move(names)), Batches(std::move(batches)) {
        for (size_t i = 0; i < Names.size(); ++i) Cols.push_back({Names[i], types[i]});
        Schema_ = TSchema{Cols};
    }
    const TSchema& Schema() const override { return Schema_; }
    bool Next(TRowSet& rowSet) override {
        if (Index >= Batches.size()) return false;
        rowSet = Batches[Index++];
        return true;
    }
};

std::unique_ptr<IRuntimeNode> Plan(const std::string& sexp, ISource& src) {
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    TParser parser;
    for (auto& [name, fn] : MakeRelParsers(std::move(opts))) parser.NodeParsers[name] = std::move(fn);
    std::istringstream in(sexp);
    TTokenStream ts(in);
    auto parsed = parser.Parse(ts);
    if (!parsed) throw std::runtime_error(parsed.error().ToString());
    auto root = std::static_pointer_cast<IOperator>(*parsed);
    AnnotateTypes(root);
    ApplyColumnPruning(root);
    TPhysicalPlanner planner;
    return planner.Build(root);
}

} // namespace

TEST(AggregateF64, SumMinMaxOverFloatColumn) {
    // key (i64), v (f64). Exact f64 values -> sums are exact (no rounding).
    std::array<int64_t, 4> k = {1, 2, 1, 2};
    std::array<double, 4> v = {1.5, 10.0, 2.5, 0.25};
    std::array<TColumn, 2> cols = {
        TColumn{.Data = reinterpret_cast<char*>(k.data())},
        TColumn{.Data = reinterpret_cast<char*>(v.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 2, .RowCount = 4, .RefCount = 1};
    TStubSource src({"k", "v"},
        {std::make_shared<TIntegerType>(TIntegerType::I64), std::make_shared<TFloatType>()},
        {batch});

    auto plan = Plan(
        "(rel aggregate (rel source \"L\") (keys k) "
        "(agg s sum v) (agg mn min v) (agg mx max v))", src);

    // Output: k (i64), s/mn/mx (f64).
    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 4u);
    EXPECT_TRUE(TMaybeType<TFloatType>(outType->Fields[1].second));

    struct TStats { double s, mn, mx; };
    std::map<int64_t, TStats> got;
    TRowSet out{};
    while (plan->Next(out)) {
        const auto* kc = reinterpret_cast<const int64_t*>(out.Columns[0].Data);
        const auto* s = reinterpret_cast<const double*>(out.Columns[1].Data);
        const auto* mn = reinterpret_cast<const double*>(out.Columns[2].Data);
        const auto* mx = reinterpret_cast<const double*>(out.Columns[3].Data);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got[kc[i]] = {s[i], mn[i], mx[i]};
        }
        Release(&out);
    }

    ASSERT_EQ(got.size(), 2u);
    EXPECT_DOUBLE_EQ(got[1].s, 4.0);   // 1.5 + 2.5
    EXPECT_DOUBLE_EQ(got[1].mn, 1.5);
    EXPECT_DOUBLE_EQ(got[1].mx, 2.5);
    EXPECT_DOUBLE_EQ(got[2].s, 10.25); // 10.0 + 0.25
    EXPECT_DOUBLE_EQ(got[2].mn, 0.25);
    EXPECT_DOUBLE_EQ(got[2].mx, 10.0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
