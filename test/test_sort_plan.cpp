#include <gtest/gtest.h>

#include <qdb/io/schema.h>
#include <qdb/exec/planner.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/passes/column_pruning.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <expected>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

using namespace NQdb;

namespace {

struct TStubSource : ISource {
    explicit TStubSource(std::vector<TColumnSchema> columns)
        : Cols_(std::move(columns))
        , Schema_{Cols_}
    {}

    const TSchema& Schema() const override { return Schema_; }
    bool Next(TRowSet&) override { return false; }

    std::vector<TColumnSchema> Cols_;
    TSchema Schema_;
};

struct TVectorSource : ISource {
    TVectorSource(std::vector<TColumnSchema> columns, std::vector<TRowSet> batches)
        : Cols_(std::move(columns))
        , Schema_{Cols_}
        , Batches_(std::move(batches))
    {}

    const TSchema& Schema() const override { return Schema_; }

    bool Next(TRowSet& rowSet) override {
        if (Cursor_ >= Batches_.size()) {
            return false;
        }
        rowSet = Batches_[Cursor_++];
        return true;
    }

    std::vector<TColumnSchema> Cols_;
    TSchema Schema_;
    std::vector<TRowSet> Batches_;
    size_t Cursor_ = 0;
};

std::expected<TOperatorPtr, NQumir::TError> BuildSqlPlan(std::string_view sql, ISource& source) {
    std::istringstream in{std::string(sql)};
    NSql::TTokenStream ts(in);
    NSql::TParser parser;
    auto parsed = parser.Parse(ts);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    auto factory = [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        return std::make_shared<TSourceOperator>(source, std::string(table));
    };

    return BuildPlan(parsed.value(), factory);
}

TRowSet MakeStringI64Batch(const std::string& names, int64_t* offsets, int64_t* values,
    int64_t rows, std::vector<TColumn>& columns, uint8_t* selection = nullptr)
{
    columns = {
        TColumn{.Data = const_cast<char*>(names.data()), .Offsets = offsets, .OffsetWidth = 8},
        TColumn{.Data = reinterpret_cast<char*>(values)},
    };
    return TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = rows,
        .Selection = selection,
        .RefCount = 1,
    };
}

TRowSet MakeI64I64Batch(int64_t* left, int64_t* right, int64_t rows,
    std::vector<TColumn>& columns)
{
    columns = {
        TColumn{.Data = reinterpret_cast<char*>(left)},
        TColumn{.Data = reinterpret_cast<char*>(right)},
    };
    return TRowSet{
        .Columns = columns.data(),
        .ColumnCount = 2,
        .RowCount = rows,
        .RefCount = 1,
    };
}

std::string StringCell(const TColumn& column, int64_t row) {
    const auto* offsets = static_cast<const int64_t*>(column.Offsets);
    return std::string(column.Data + offsets[row], column.Data + offsets[row + 1]);
}

NQumir::NAst::TExprPtr Ident(std::string name) {
    return std::make_shared<NQumir::NAst::TIdentExpr>(
        NQumir::TLocation{}, std::move(name));
}

std::vector<std::string> FieldNames(const NQumir::NAst::TTypePtr& type) {
    auto* st = static_cast<NQumir::NAst::TStructType*>(type.get());
    std::vector<std::string> names;
    if (!st) {
        return names;
    }
    for (const auto& [name, _] : st->Fields) {
        names.push_back(name);
    }
    return names;
}

} // namespace

TEST(SortPlan, BuildPlanWrapsOrderByAfterProjection) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TStubSource source({{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan("SELECT a AS k FROM t ORDER BY k DESC NULLS LAST", source);
    ASSERT_TRUE(plan.has_value()) << (plan ? "" : plan.error().ToString());

    auto sort = TMaybeOp<TSortOperator>(*plan);
    ASSERT_TRUE(sort);
    ASSERT_EQ(sort.Cast()->Keys().size(), 1u);
    EXPECT_EQ(sort.Cast()->Keys()[0].Column, "k");
    EXPECT_EQ(sort.Cast()->Keys()[0].Direction, ESortDirection::Desc);
    EXPECT_EQ(sort.Cast()->Keys()[0].Nulls, ESortNulls::Last);

    auto project = TMaybeOp<TProjectOperator>(sort.Cast()->Input());
    ASSERT_TRUE(project);
    ASSERT_EQ(project.Cast()->Projections().size(), 1u);
    EXPECT_EQ(project.Cast()->Projections()[0].Name, "k");
}

TEST(SortPlan, BuildPlanRejectsOrderByExpressionForMvp) {
    auto i64 = std::make_shared<NQumir::NAst::TIntegerType>();
    TStubSource source({{"a", i64}, {"b", i64}});
    auto plan = BuildSqlPlan("SELECT a FROM t ORDER BY a + 1", source);
    ASSERT_FALSE(plan.has_value());
    EXPECT_NE(plan.error().ToString().find("ORDER BY currently supports only output column identifiers"),
        std::string::npos);
}

TEST(SortPlan, ColumnPruningKeepsSortKeyColumns) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    TStubSource source({{"a", i64}, {"b", i64}, {"c", i64}});
    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto sort = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "b", .Direction = ESortDirection::Asc},
    });
    auto project = std::make_shared<TProjectOperator>(sort, std::vector<TProjectionSpec>{
        {.Name = "a", .Expression = Ident("a")},
    });

    ApplyColumnPruning(project);

    EXPECT_EQ(FieldNames(sourceOp->OutputColumns()), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(FieldNames(sort->RequiredColumns()), (std::vector<std::string>{"a", "b"}));
}

TEST(SortExec, SortsStringAndNumericKeysAcrossBatches) {
    using namespace NQumir::NAst;

    std::string data1 = "bobalice";
    int64_t offsets1[] = {0, 3, 8};
    int64_t values1[] = {2, 1};
    std::vector<TColumn> columns1;
    TRowSet batch1 = MakeStringI64Batch(data1, offsets1, values1, 2, columns1);

    std::string data2 = "carolalice";
    int64_t offsets2[] = {0, 5, 10};
    int64_t values2[] = {3, 4};
    std::vector<TColumn> columns2;
    TRowSet batch2 = MakeStringI64Batch(data2, offsets2, values2, 2, columns2);

    auto str = std::make_shared<TStringType>();
    auto i64 = std::make_shared<TIntegerType>();
    TVectorSource source(
        {{"name", str}, {"score", i64}},
        {batch1, batch2});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto root = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "name", .Direction = ESortDirection::Asc},
        {.Column = "score", .Direction = ESortDirection::Desc},
    });

    TPhysicalPlanner planner;
    auto runtime = planner.Build(root);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 4);
    EXPECT_EQ(StringCell(out.Columns[0], 0), "alice");
    EXPECT_EQ(reinterpret_cast<int64_t*>(out.Columns[1].Data)[0], 4);
    EXPECT_EQ(StringCell(out.Columns[0], 1), "alice");
    EXPECT_EQ(reinterpret_cast<int64_t*>(out.Columns[1].Data)[1], 1);
    EXPECT_EQ(StringCell(out.Columns[0], 2), "bob");
    EXPECT_EQ(StringCell(out.Columns[0], 3), "carol");
    Release(&out);

    TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

TEST(SortExec, SortsCompositeNumericKeysWithFusedRadixKernel) {
    using namespace NQumir::NAst;

    int64_t a1[] = {2, 1, 2};
    int64_t b1[] = {10, 5, 30};
    std::vector<TColumn> columns1;
    TRowSet batch1 = MakeI64I64Batch(a1, b1, 3, columns1);

    int64_t a2[] = {1, 2};
    int64_t b2[] = {7, 20};
    std::vector<TColumn> columns2;
    TRowSet batch2 = MakeI64I64Batch(a2, b2, 2, columns2);

    auto i64 = std::make_shared<TIntegerType>();
    TVectorSource source(
        {{"a", i64}, {"b", i64}},
        {batch1, batch2});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto root = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "a", .Direction = ESortDirection::Asc},
        {.Column = "b", .Direction = ESortDirection::Desc},
    });

    TPhysicalPlanner planner;
    auto runtime = planner.Build(root);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 5);
    auto* outA = reinterpret_cast<int64_t*>(out.Columns[0].Data);
    auto* outB = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outA, outA + out.RowCount),
        (std::vector<int64_t>{1, 1, 2, 2, 2}));
    EXPECT_EQ(std::vector<int64_t>(outB, outB + out.RowCount),
        (std::vector<int64_t>{7, 5, 30, 20, 10}));
    Release(&out);
}

TEST(SortExec, ProjectionAfterSortSeesSortedRows) {
    using namespace NQumir::NAst;

    int64_t a[] = {100, 200, 300, 400};
    int64_t b[] = {3, 1, 4, 2};
    std::vector<TColumn> columns;
    TRowSet batch = MakeI64I64Batch(a, b, 4, columns);

    auto i64 = std::make_shared<TIntegerType>();
    TVectorSource source(
        {{"a", i64}, {"b", i64}},
        {batch});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto sort = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "b", .Direction = ESortDirection::Asc},
    });
    auto project = std::make_shared<TProjectOperator>(sort, std::vector<TProjectionSpec>{
        {.Name = "a", .Expression = Ident("a")},
    });

    TPhysicalPlanner planner;
    auto runtime = planner.Build(project);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.ColumnCount, 1);
    ASSERT_EQ(out.RowCount, 4);
    auto* outA = reinterpret_cast<int64_t*>(out.Columns[0].Data);
    EXPECT_EQ(std::vector<int64_t>(outA, outA + out.RowCount),
        (std::vector<int64_t>{200, 400, 100, 300}));
    Release(&out);
}

TEST(SortExec, SortAfterAggregate) {
    using namespace NQumir::NAst;

    int64_t keys[] = {2, 1, 2, 3, 1};
    int64_t values[] = {10, 5, 7, 4, 6};
    std::vector<TColumn> columns;
    TRowSet batch = MakeI64I64Batch(keys, values, 5, columns);

    auto i64 = std::make_shared<TIntegerType>();
    TVectorSource source(
        {{"k", i64}, {"v", i64}},
        {batch});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto aggregate = std::make_shared<TAggregateOperator>(
        sourceOp,
        std::vector<std::string>{"k"},
        std::vector<TAggregateSpec>{{
            .Name = "s",
            .Func = "sum",
            .Arg = Ident("v"),
        }});
    auto sort = std::make_shared<TSortOperator>(aggregate, std::vector<TSortKey>{
        {.Column = "s", .Direction = ESortDirection::Desc},
    });

    TPhysicalPlanner planner;
    auto runtime = planner.Build(sort);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.ColumnCount, 2);
    ASSERT_EQ(out.RowCount, 3);
    auto* outK = reinterpret_cast<int64_t*>(out.Columns[0].Data);
    auto* outS = reinterpret_cast<int64_t*>(out.Columns[1].Data);
    EXPECT_EQ(std::vector<int64_t>(outK, outK + out.RowCount),
        (std::vector<int64_t>{2, 1, 3}));
    EXPECT_EQ(std::vector<int64_t>(outS, outS + out.RowCount),
        (std::vector<int64_t>{17, 11, 4}));
    Release(&out);
}

TEST(SortExec, RespectsInputSelection) {
    using namespace NQumir::NAst;

    std::string data = "cab";
    int64_t offsets[] = {0, 1, 2, 3};
    int64_t values[] = {3, 1, 2};
    uint8_t selection[] = {1, 0, 1};
    std::vector<TColumn> columns;
    TRowSet batch = MakeStringI64Batch(data, offsets, values, 3, columns, selection);

    auto str = std::make_shared<TStringType>();
    auto i64 = std::make_shared<TIntegerType>();
    TVectorSource source(
        {{"name", str}, {"score", i64}},
        {batch});

    auto sourceOp = std::make_shared<TSourceOperator>(source, "t");
    auto root = std::make_shared<TSortOperator>(sourceOp, std::vector<TSortKey>{
        {.Column = "score", .Direction = ESortDirection::Asc},
    });

    TPhysicalPlanner planner;
    auto runtime = planner.Build(root);

    TRowSet out{};
    ASSERT_TRUE(runtime->Next(out));
    ASSERT_EQ(out.RowCount, 2);
    EXPECT_EQ(reinterpret_cast<int64_t*>(out.Columns[1].Data)[0], 2);
    EXPECT_EQ(reinterpret_cast<int64_t*>(out.Columns[1].Data)[1], 3);
    EXPECT_EQ(StringCell(out.Columns[0], 0), "b");
    EXPECT_EQ(StringCell(out.Columns[0], 1), "c");
    Release(&out);
}

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
