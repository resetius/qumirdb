#include <gtest/gtest.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include <qdb/exec/executor.h>
#include <qdb/exec/planner.h>
#include <qdb/io/io.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/sexp/parser.h>

#include <array>
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
        for (size_t i = 0; i < Names.size(); ++i) {
            Cols.push_back({Names[i], types[i]});
        }
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
    for (auto& [name, fn] : MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[name] = std::move(fn);
    }
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

TEST(ProjectE2E, IdentZeroCopyPlusComputed) {
    std::array<double, 3> x = {10.0, 20.0, 4.0};
    std::array<double, 3> z = {0.5, 0.25, 0.75};
    std::array<double, 3> w = {7.0, 8.0, 9.0}; // unused column -> pruned away
    std::array<TColumn, 3> cols = {
        TColumn{.Data = reinterpret_cast<char*>(x.data())},
        TColumn{.Data = reinterpret_cast<char*>(z.data())},
        TColumn{.Data = reinterpret_cast<char*>(w.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 3, .RowCount = 3, .RefCount = 1};
    TStubSource src({"x", "z", "w"},
        {std::make_shared<TFloatType>(), std::make_shared<TFloatType>(),
         std::make_shared<TFloatType>()},
        {batch});

    // kept_x is a zero-copy ident; c is computed x * (2.0 - z).
    auto plan = Plan(
        "(rel project (rel source \"L\") (kept_x x) (c (* x (- (: 2.0 f64) z))))", src);

    // Output schema: kept_x (f64), c (f64).
    auto* outType = static_cast<TStructType*>(plan->OutputType().get());
    ASSERT_EQ(outType->Fields.size(), 2u);
    EXPECT_EQ(outType->Fields[0].first, "kept_x");
    EXPECT_EQ(outType->Fields[1].first, "c");
    EXPECT_TRUE(TMaybeType<TFloatType>(outType->Fields[1].second));

    TRowSet out{};
    ASSERT_TRUE(plan->Next(out));
    ASSERT_EQ(out.ColumnCount, 2);
    ASSERT_EQ(out.RowCount, 3);
    const auto* keptX = reinterpret_cast<const double*>(out.Columns[0].Data);
    const auto* c = reinterpret_cast<const double*>(out.Columns[1].Data);
    // kept_x is zero-copy: points into the input column.
    EXPECT_EQ(out.Columns[0].Data, reinterpret_cast<const char*>(x.data()));
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(keptX[i], x[i]);
        EXPECT_DOUBLE_EQ(c[i], x[i] * (2.0 - z[i]));
    }
    Release(&out);
    EXPECT_FALSE(plan->Next(out));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
