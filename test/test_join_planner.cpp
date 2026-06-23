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

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace NQdb;
using namespace NQdb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {

struct TVectorSource : ISource {
    std::vector<std::string> Names;
    std::vector<TColumnSchema> Cols;
    TSchema Schema_;
    std::vector<TRowSet> Batches;
    size_t Index = 0;

    std::vector<TTypePtr> Types;

    TVectorSource(std::vector<std::string> names, std::vector<TRowSet> batches,
        std::vector<TTypePtr> types = {})
        : Names(std::move(names)), Batches(std::move(batches)), Types(std::move(types)) {
        for (size_t i = 0; i < Names.size(); ++i) {
            Cols.push_back({Names[i], i < Types.size() ? Types[i]
                : std::make_shared<TIntegerType>(TIntegerType::I64)});
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

namespace {

TTypePtr I32() { return std::make_shared<TIntegerType>(TIntegerType::I32); }
TTypePtr I64T() { return std::make_shared<TIntegerType>(TIntegerType::I64); }

} // namespace

TEST(JoinPlanner, Int32KeyE2E) {
    // int32 join key (the TPC-H *_key case) flows through the generic path.
    std::vector<int32_t> lk = {1, 2, 1}; std::vector<int64_t> lv = {10, 20, 30};
    std::vector<int32_t> rk = {1, 1, 3}; std::vector<int64_t> rv = {100, 200, 300};
    std::vector<TColumn> lcols = {TColumn{.Data = reinterpret_cast<char*>(lk.data())},
                                  TColumn{.Data = reinterpret_cast<char*>(lv.data())}};
    std::vector<TColumn> rcols = {TColumn{.Data = reinterpret_cast<char*>(rk.data())},
                                  TColumn{.Data = reinterpret_cast<char*>(rv.data())}};
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};
    TRowSet rbatch{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};

    TVectorSource left({"lk", "lv"}, {lbatch}, {I32(), I64T()});
    TVectorSource right({"rk", "rv"}, {rbatch}, {I32(), I64T()});

    auto plan = PlanJoin(
        "(rel join (rel source \"L\") (rel source \"R\") ((lk rk)) (inner))", left, right);

    std::vector<std::tuple<int64_t, int64_t>> got; // (lv, rv) of matched rows
    TRowSet out{};
    while (plan->Next(out)) {
        ASSERT_EQ(out.ColumnCount, 4); // lk(i32), lv(i64), rk(i32), rv(i64)
        for (int64_t i = 0; i < out.RowCount; ++i) {
            // Keys equal: out col0 (lk i32) == out col2 (rk i32).
            EXPECT_EQ(reinterpret_cast<const int32_t*>(out.Columns[0].Data)[i],
                      reinterpret_cast<const int32_t*>(out.Columns[2].Data)[i]);
            got.emplace_back(reinterpret_cast<const int64_t*>(out.Columns[1].Data)[i],
                             reinterpret_cast<const int64_t*>(out.Columns[3].Data)[i]);
        }
        Release(&out);
    }
    std::vector<std::tuple<int64_t, int64_t>> expected = {
        {10, 100}, {10, 200}, {30, 100}, {30, 200}};
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(got, expected);
}

TEST(JoinPlanner, CompositeI32KeyE2E) {
    // Composite (i32, i32) key.
    std::vector<int32_t> la = {1, 1, 2}; std::vector<int32_t> lb = {7, 8, 7};
    std::vector<int32_t> ra = {1, 2, 1}; std::vector<int32_t> rb = {7, 7, 9};
    std::vector<TColumn> lcols = {TColumn{.Data = reinterpret_cast<char*>(la.data())},
                                  TColumn{.Data = reinterpret_cast<char*>(lb.data())}};
    std::vector<TColumn> rcols = {TColumn{.Data = reinterpret_cast<char*>(ra.data())},
                                  TColumn{.Data = reinterpret_cast<char*>(rb.data())}};
    TRowSet lbatch{.Columns = lcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};
    TRowSet rbatch{.Columns = rcols.data(), .ColumnCount = 2, .RowCount = 3, .RefCount = 1};

    TVectorSource left({"la", "lb"}, {lbatch}, {I32(), I32()});
    TVectorSource right({"ra", "rb"}, {rbatch}, {I32(), I32()});

    auto plan = PlanJoin(
        "(rel join (rel source \"L\") (rel source \"R\") ((la ra) (lb rb)) (inner))",
        left, right);

    int64_t rows = 0;
    TRowSet out{};
    while (plan->Next(out)) {
        for (int64_t i = 0; i < out.RowCount; ++i) {
            // Both key components equal.
            EXPECT_EQ(reinterpret_cast<const int32_t*>(out.Columns[0].Data)[i],
                      reinterpret_cast<const int32_t*>(out.Columns[2].Data)[i]);
            EXPECT_EQ(reinterpret_cast<const int32_t*>(out.Columns[1].Data)[i],
                      reinterpret_cast<const int32_t*>(out.Columns[3].Data)[i]);
        }
        rows += out.RowCount;
        Release(&out);
    }
    // (1,7)x(1,7) and (2,7)x(2,7) match -> 2 rows.
    EXPECT_EQ(rows, 2);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
