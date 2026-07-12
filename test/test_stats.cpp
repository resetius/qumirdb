#include <gtest/gtest.h>

#include <qdb/io/parquet/source.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/stats.h>
#include <qdb/plan/pipeline.h>

#include <qumir/parser/type.h>

#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

using NQdb::TStats;
using TColumnStats = NQdb::TStats::TColumnStats;

namespace {

// Build a histogram (raw uint64 bit patterns) from typed boundary values.
template<typename T>
std::vector<uint64_t> Hist(std::initializer_list<T> values) {
    std::vector<uint64_t> out;
    out.reserve(values.size());
    for (T v : values) {
        out.push_back(std::bit_cast<uint64_t>(v));
    }
    return out;
}

void WriteParquet(const std::string& path, std::shared_ptr<arrow::RecordBatch> batch) {
    auto outfile = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
    auto status = parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), outfile, /*chunk*/ 1024);
    ASSERT_TRUE(status.ok()) << status.ToString();
}

} // namespace

// ─── TColumnStats scalar accessors: bit_cast round-trip ──────────────────────

TEST(StatsTest, MinMaxRoundTrip) {
    TColumnStats cs;
    cs.MinValue = std::bit_cast<uint64_t>(int64_t{-7});
    cs.MaxValue = std::bit_cast<uint64_t>(int64_t{42});
    EXPECT_EQ(cs.GetMinValue<int64_t>().value(), -7);
    EXPECT_EQ(cs.GetMaxValue<int64_t>().value(), 42);

    TColumnStats fd;
    fd.MinValue = std::bit_cast<uint64_t>(double{0.5});
    fd.MaxValue = std::bit_cast<uint64_t>(double{3.5});
    EXPECT_DOUBLE_EQ(fd.GetMinValue<double>().value(), 0.5);
    EXPECT_DOUBLE_EQ(fd.GetMaxValue<double>().value(), 3.5);

    TColumnStats empty;
    EXPECT_FALSE(empty.GetMinValue<int64_t>().has_value());
}

// ─── FractionBelow: equi-depth histogram selectivity ─────────────────────────

TEST(StatsTest, FractionBelowUniformInt) {
    // 100 buckets over [0, 1000): boundaries 0,10,...,1000.
    TColumnStats cs;
    std::vector<uint64_t> h;
    for (int64_t v = 0; v <= 1000; v += 10) h.push_back(std::bit_cast<uint64_t>(v));
    cs.Histogram = std::move(h);

    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(-1).value(), 0.0);   // below min
    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(1000).value(), 1.0); // at/above max
    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(500).value(), 0.5);  // boundary
    EXPECT_DOUBLE_EQ(cs.FractionBelow<int64_t>(250).value(), 0.25);
    EXPECT_NEAR(cs.FractionBelow<int64_t>(505).value(), 0.505, 1e-9); // mid-bucket interp
}

TEST(StatsTest, FractionBelowDouble) {
    TColumnStats cs;
    cs.Histogram = Hist<double>({0.0, 0.5, 1.0}); // 2 buckets
    EXPECT_DOUBLE_EQ(cs.FractionBelow<double>(0.5).value(), 0.5);
    EXPECT_DOUBLE_EQ(cs.FractionBelow<double>(0.25).value(), 0.25);
    EXPECT_DOUBLE_EQ(cs.FractionBelow<double>(-1.0).value(), 0.0);
    EXPECT_DOUBLE_EQ(cs.FractionBelow<double>(2.0).value(), 1.0);
}

TEST(StatsTest, FractionBelowRepeatedBoundaries) {
    // Heavy low value: several buckets collapse at 0 (low-cardinality column).
    TColumnStats cs;
    cs.Histogram = Hist<int64_t>({0, 0, 0, 10, 20}); // 4 buckets
    // No divide-by-zero on the repeated [0,0] buckets; monotone result.
    const double at0 = cs.FractionBelow<int64_t>(0).value();
    const double at5 = cs.FractionBelow<int64_t>(5).value();
    const double at15 = cs.FractionBelow<int64_t>(15).value();
    EXPECT_GE(at0, 0.0);
    EXPECT_LE(at0, at5);
    EXPECT_LE(at5, at15);
    EXPECT_LE(at15, 1.0);
}

TEST(StatsTest, FractionBelowEmptyHistogram) {
    TColumnStats cs;
    EXPECT_FALSE(cs.FractionBelow<int64_t>(5).has_value());
}

// ─── TParquetSource::Stats(): row count + standard column stats ──────────────

TEST(StatsTest, ParquetStandardStats) {
    const std::string path = "/tmp/test_stats_qdb.parquet";

    arrow::Int64Builder id;
    arrow::DoubleBuilder val;
    arrow::StringBuilder name;
    (void)id.AppendValues({1, 2, 3, 4});
    (void)val.AppendValues({1.5, 2.5, 3.5, 0.5});
    (void)name.Append("a");
    (void)name.Append("b");
    (void)name.AppendNull();
    (void)name.Append("d");

    auto schema = arrow::schema({
        arrow::field("id", arrow::int64(), /*nullable*/ false),
        arrow::field("val", arrow::float64(), false),
        arrow::field("name", arrow::utf8(), /*nullable*/ true),
    });
    auto batch = arrow::RecordBatch::Make(schema, 4, {
        id.Finish().ValueOrDie(),
        val.Finish().ValueOrDie(),
        name.Finish().ValueOrDie(),
    });
    WriteParquet(path, batch);

    NQdb::TParquetSource source(path);
    auto st = source.Stats();
    ASSERT_TRUE(st != nullptr);          // always present: parquet carries num_rows
    EXPECT_EQ(st->RowCount, 4u);

    const auto& idStats = st->ColumnStats.at("id");
    EXPECT_EQ(idStats->GetMinValue<int64_t>().value(), 1);
    EXPECT_EQ(idStats->GetMaxValue<int64_t>().value(), 4);
    EXPECT_EQ(idStats->NullCount.value(), 0u);

    const auto& valStats = st->ColumnStats.at("val");
    EXPECT_DOUBLE_EQ(valStats->GetMinValue<double>().value(), 0.5);
    EXPECT_DOUBLE_EQ(valStats->GetMaxValue<double>().value(), 3.5);

    const auto& nameStats = st->ColumnStats.at("name");
    EXPECT_EQ(nameStats->NullCount.value(), 1u);   // one null aggregated
    EXPECT_FALSE(nameStats->MinValue.has_value());  // string: no numeric min/max
}

// ─── EstimateStats: filter selectivity propagation (source → filter) ─────────
// NOTE: exercises the whole stats-propagation pipeline. Expected to go green
// once EstimateStats actually computes per-node stats (calls ComputeStatsFor),
// the source's per-column stats are keyed by the qualified name the plan uses,
// and range predicates dispatch FractionBelow on the column's real type.

namespace {

// Source with known column statistics (bare column names, like TParquetSource).
struct TStatsSource : NQdb::ISource {
    std::vector<std::string> Names{"x"};
    std::vector<NQdb::TColumnSchema> Cols;
    NQdb::TSchema Schema_;
    NQdb::TStatsPtr Stats_;

    TStatsSource() {
        using NQumir::NAst::TIntegerType;
        Cols.push_back({Names[0], std::make_shared<TIntegerType>(TIntegerType::I64)});
        Schema_ = NQdb::TSchema{Cols};

        Stats_ = std::make_shared<TStats>();
        Stats_->RowCount = 1000;
        auto x = std::make_shared<TColumnStats>();
        x->Ndv = 100;
        x->MinValue = std::bit_cast<uint64_t>(int64_t{0});
        x->MaxValue = std::bit_cast<uint64_t>(int64_t{999});
        for (int64_t v = 0; v <= 1000; v += 10) {           // uniform, 100 buckets
            x->Histogram.push_back(std::bit_cast<uint64_t>(v));
        }
        Stats_->ColumnStats["x"] = std::move(x);            // bare name
    }

    const NQdb::TSchema& Schema() const override { return Schema_; }
    const NQdb::TStatsPtr Stats() const override { return Stats_; }
    bool Next(NQdb::TRowSet&) override { return false; }
};

} // namespace

TEST(StatsTest, FilterSelectivityPropagation) {
    TStatsSource src;
    NQdb::TOperatorPtr plan = std::make_shared<NQdb::TSourceOperator>(src, "t");

    auto filter = NQdb::MakeFilter(plan, "(< x 250)");
    ASSERT_TRUE(filter.has_value()) << filter.error().ToString();
    plan = *filter;

    NQdb::ApplyPlanPasses(plan);

    ASSERT_TRUE(plan->Stats_ != nullptr) << "filter got no propagated stats";
    // x < 250 over uniform [0,1000) => ~25% of 1000 rows.
    EXPECT_GE(plan->Stats_->RowCount, 200u);
    EXPECT_LE(plan->Stats_->RowCount, 300u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
