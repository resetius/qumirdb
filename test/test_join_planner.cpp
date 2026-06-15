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

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace NQqb;
using namespace NQqb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {

struct TVectorSource : ISource {
    std::vector<std::string> Names;
    std::vector<TColumnSchema> Cols;
    TSchema Schema_;
    std::vector<TRowSet> Batches;
    size_t Index = 0;

    TVectorSource(std::vector<std::string> names, std::vector<TRowSet> batches)
        : Names(std::move(names)), Batches(std::move(batches)) {
        for (auto& n : Names) {
            Cols.push_back({n, std::make_shared<TIntegerType>(TIntegerType::I64)});
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

TRowSet KeyValBatch(int64_t* keys, int64_t* vals, int64_t rows, std::vector<TColumn>& cols) {
    cols = {TColumn{.Data = reinterpret_cast<char*>(keys)},
            TColumn{.Data = reinterpret_cast<char*>(vals)}};
    return TRowSet{.Columns = cols.data(), .ColumnCount = 2, .RowCount = rows, .RefCount = 1};
}

// Parses `sexp`, wiring "L" -> left source, anything else -> right source, then
// runs the full logical pipeline + physical planner.
std::unique_ptr<IRuntimeNode> PlanJoin(
    const std::string& sexp, ISource& left, ISource& right) {
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        ISource& src = (path == "L") ? left : right;
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

TEST(JoinPlanner, InnerJoinE2E) {
    std::vector<int64_t> lk = {1, 2, 1}, lv = {10, 20, 30};
    std::vector<int64_t> rk = {1, 1, 3}, rv = {100, 200, 300};
    std::vector<TColumn> lcols, rcols;
    TVectorSource left({"lk", "lv"}, {KeyValBatch(lk.data(), lv.data(), 3, lcols)});
    TVectorSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 3, rcols)});

    auto plan = PlanJoin(
        "(rel join (rel source \"L\") (rel source \"R\") ((lk rk)) (inner))",
        left, right);

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> got;
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 4);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[2].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i]);
        }
        Release(&out);
    }

    std::vector<std::tuple<int64_t, int64_t, int64_t, int64_t>> expected = {
        {1, 10, 1, 100}, {1, 10, 1, 200}, {1, 30, 1, 100}, {1, 30, 1, 200}};
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(JoinPlanner, ProjectOnTopPrunesJoinInputs) {
    // Project keeps only lk and rv; lv and rk(beyond the key) are not selected.
    // Pruning narrows each source, but the key columns survive.
    std::vector<int64_t> lk = {5, 6, 5}, lv = {10, 20, 30};
    std::vector<int64_t> rk = {5, 5, 7}, rv = {100, 200, 300};
    std::vector<TColumn> lcols, rcols;
    TVectorSource left({"lk", "lv"}, {KeyValBatch(lk.data(), lv.data(), 3, lcols)});
    TVectorSource right({"rk", "rv"}, {KeyValBatch(rk.data(), rv.data(), 3, rcols)});

    auto plan = PlanJoin(
        "(rel project (rel join (rel source \"L\") (rel source \"R\") "
        "((lk rk)) (inner)) (a lk) (b rv))",
        left, right);

    std::vector<std::tuple<int64_t, int64_t>> got;
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 2);
        for (int64_t i = 0; i < out.RowCount; ++i) {
            got.emplace_back(
                reinterpret_cast<const int64_t*>(out.Columns[0].Data)[i],
                reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i]);
        }
        Release(&out);
    }

    // lk==rk matches: l0(5),l2(5) x r0(5),r1(5) -> 4 rows, each (lk=5, rv in {100,200}).
    std::vector<std::tuple<int64_t, int64_t>> expected = {
        {5, 100}, {5, 200}, {5, 100}, {5, 200}};
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
