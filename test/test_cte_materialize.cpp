#include <gtest/gtest.h>
#include "mock_source.h"
#include "plan_runner.h"

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <qdb/plan/build.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/pipeline.h>
#include <qdb/sql/parser.h>

#include <cstdint>
#include <expected>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace NQdb;

namespace {

// Backing storage for an in-memory source split across several batches; the
// TRowSet columns point into Keys/Vals, so it must outlive the run.
struct TTable {
    std::vector<int64_t> Keys;
    std::vector<int64_t> Vals;
    std::vector<std::vector<TColumn>> Columns;
    std::vector<TRowSet> Batches;
};

std::unique_ptr<TTable> MakeTable(int64_t rows, size_t batches) {
    auto t = std::make_unique<TTable>();
    for (int64_t i = 0; i < rows; ++i) {
        t->Keys.push_back(i + 1);
        t->Vals.push_back((i + 1) * 10);
    }
    t->Columns.resize(batches);
    const size_t perBatch = batches ? (t->Keys.size() + batches - 1) / batches : 0;
    for (size_t b = 0; b < batches; ++b) {
        const size_t off = b * perBatch;
        if (off >= t->Keys.size() && b > 0) {
            t->Columns[b] = {};
            continue;
        }
        const size_t count = std::min(perBatch, t->Keys.size() - std::min(off, t->Keys.size()));
        t->Columns[b] = {
            TColumn{.Data = reinterpret_cast<char*>(t->Keys.data() + off)},
            TColumn{.Data = reinterpret_cast<char*>(t->Vals.data() + off)},
        };
        t->Batches.push_back(TRowSet{
            .Columns = t->Columns[b].data(),
            .ColumnCount = 2,
            .RowCount = static_cast<int64_t>(count),
            .Selection = nullptr,
            .Destroy = nullptr,
            .Private = nullptr,
            .RefCount = 1,
        });
    }
    return t;
}

std::unique_ptr<TTestRuntime> RunSql(
    const std::string& sql,
    const std::unordered_map<std::string, ISource*>& sources,
    NScheduler::TSettings settings = {})
{
    std::istringstream in(sql);
    NSql::TTokenStream ts(in);
    NSql::TParser parser;
    auto parsed = parser.Parse(ts);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    auto root = BuildPlan(*parsed, [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        auto it = sources.find(std::string(table));
        if (it == sources.end()) {
            return std::unexpected(NQumir::TError("unknown table: " + std::string(table)));
        }
        return std::make_shared<TSourceOperator>(*it->second, std::string(table));
    });
    if (!root) {
        throw std::runtime_error(root.error().ToString());
    }
    TOperatorPtr plan = *root;
    ApplyPlanPasses(plan);
    return RunPlan(plan, settings);
}

int64_t CountRows(TTestRuntime& runtime) {
    int64_t total = 0;
    TRowSet batch{};
    while (runtime.Next(batch)) {
        total += batch.RowCount;
        Release(&batch);
        batch = {};
    }
    return total;
}

} // namespace

// A source drained more than once yields nothing the second time, so a correct
// self-join result over a single-use mock proves the definition was materialized
// once (inlining would leave the second reference empty -> 0 join rows).
TEST(CteMaterialize, DiamondSelfJoinRunsDefinitionOnce) {
    auto table = MakeTable(/*rows=*/12, /*batches=*/3);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql(
        "WITH x AS (SELECT k, v FROM t) "
        "SELECT p.k, q.v FROM x p JOIN x q ON p.k = q.k",
        sources);
    EXPECT_EQ(CountRows(*runtime), 12);
}

// The diamond is the classic multicast-deadlock shape; with a single-slot queue
// the blocking spool must still complete (producer never blocks on a consumer).
TEST(CteMaterialize, DiamondCapacityOneNoDeadlock) {
    auto table = MakeTable(/*rows=*/40, /*batches=*/20);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    NScheduler::TSettings settings;
    settings.Queue.RowsetCapacityPerLane = 1;

    auto runtime = RunSql(
        "WITH x AS (SELECT k, v FROM t) "
        "SELECT p.k, q.v FROM x p JOIN x q ON p.k = q.k",
        sources, settings);
    EXPECT_EQ(CountRows(*runtime), 40);
}

TEST(CteMaterialize, DiamondCapacityOneThreadedNoDeadlock) {
    auto table = MakeTable(/*rows=*/40, /*batches=*/20);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    NScheduler::TSettings settings;
    settings.Queue.RowsetCapacityPerLane = 1;
    settings.Scheduler.Mode = NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 4;

    auto runtime = RunSql(
        "WITH x AS (SELECT k, v FROM t) "
        "SELECT p.k, q.v FROM x p JOIN x q ON p.k = q.k",
        sources, settings);
    EXPECT_EQ(CountRows(*runtime), 40);
}

TEST(CteMaterialize, EmptyCte) {
    auto table = MakeTable(/*rows=*/0, /*batches=*/1);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql(
        "WITH x AS (SELECT k, v FROM t) "
        "SELECT p.k, q.v FROM x p JOIN x q ON p.k = q.k",
        sources);
    EXPECT_EQ(CountRows(*runtime), 0);
}

// b is materialized (referenced twice); a is inlined inside b's producer plan,
// so a's single-use source is still scanned exactly once.
TEST(CteMaterialize, NestedMaterializedProducer) {
    auto table = MakeTable(/*rows=*/12, /*batches=*/3);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql(
        "WITH a AS (SELECT k, v FROM t), b AS (SELECT k, v FROM a) "
        "SELECT p.k, q.v FROM b p JOIN b q ON p.k = q.k",
        sources);
    EXPECT_EQ(CountRows(*runtime), 12);
}

// Both UNION ALL branches read the full shared spool independently.
TEST(CteMaterialize, UnionAllTwoConsumers) {
    auto table = MakeTable(/*rows=*/12, /*batches=*/3);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql(
        "WITH x AS (SELECT k FROM t) "
        "SELECT k FROM x UNION ALL SELECT k FROM x",
        sources);
    EXPECT_EQ(CountRows(*runtime), 24);
}

// One consumer feeds a sort (a potentially in-place operator); it must not
// corrupt the batches the other consumer streams from the shared spool.
TEST(CteMaterialize, SortBranchDoesNotCorruptSpool) {
    auto table = MakeTable(/*rows=*/12, /*batches=*/3);
    TMockSource t({"k", "v"}, std::move(table->Batches));
    std::unordered_map<std::string, ISource*> sources = {{"t", &t}};

    auto runtime = RunSql(
        "WITH x AS (SELECT k FROM t) "
        "SELECT k FROM x "
        "UNION ALL "
        "SELECT k FROM (SELECT k FROM x ORDER BY k) s",
        sources);
    EXPECT_EQ(CountRows(*runtime), 24);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
