#include <gtest/gtest.h>

#include <qdb/io/parquet/row_id.h>
#include <qdb/io/parquet/source.h>
#include <qdb/io/text/sink.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/types/nullable.h>

#include <sstream>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

using namespace NQdb;

namespace {

void WriteParquet(
    const std::string& path,
    std::shared_ptr<arrow::RecordBatch> batch,
    int64_t chunkSize = 1024)
{
    auto outfile = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
    auto status = parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), outfile, chunkSize);
    ASSERT_TRUE(status.ok()) << status.ToString();
}

} // namespace

TEST(IOTest, ParquetRoundtrip) {
    const std::string path = "/tmp/test_io_qdb.parquet";

    arrow::Int64Builder ids;
    arrow::StringBuilder names;
    arrow::DoubleBuilder values;
    arrow::BooleanBuilder flags;

    (void)ids.AppendValues({1, 2, 3});
    (void)names.AppendValues({"alice", "bob", "carol"});
    (void)values.AppendValues({1.5, 2.5, 3.5});
    (void)flags.AppendValues(std::vector<bool>{true, false, true});

    auto schema = arrow::schema({
        arrow::field("id", arrow::int64(), false),
        arrow::field("name", arrow::utf8()),
        arrow::field("value", arrow::float64()),
        arrow::field("flag", arrow::boolean()),
    });
    auto batch = arrow::RecordBatch::Make(schema, 3, {
        ids.Finish().ValueOrDie(),
        names.Finish().ValueOrDie(),
        values.Finish().ValueOrDie(),
        flags.Finish().ValueOrDie(),
    });
    WriteParquet(path, batch);

    NQdb::TParquetSource source(path);
    const auto& s = source.Schema();

    ASSERT_EQ(s.Columns.size(), 4u);
    EXPECT_EQ(s.Columns[0].Name, "id");
    EXPECT_EQ(s.Columns[1].Name, "name");
    EXPECT_EQ(s.Columns[2].Name, "value");
    EXPECT_EQ(s.Columns[3].Name, "flag");
    EXPECT_FALSE(IsNullableType(s.Columns[0].Type));
    EXPECT_TRUE(IsNullableType(s.Columns[1].Type));
    EXPECT_TRUE(IsNullableType(s.Columns[2].Type));
    EXPECT_TRUE(IsNullableType(s.Columns[3].Type));
    EXPECT_EQ(UnwrapNullableType(s.Columns[1].Type)->TypeName(),
        NQumir::NAst::TStringType::TypeId);

    auto structType = NQumir::NAst::TMaybeType<NQumir::NAst::TStructType>(
        StructTypeFromSchema(s));
    ASSERT_TRUE(structType);
    EXPECT_EQ(structType.Cast()->Fields[1].second, s.Columns[1].Type);

    TRowSet rowSet = {};
    ASSERT_TRUE(source.Next(rowSet));
    EXPECT_EQ(rowSet.RowCount, 3);
    EXPECT_EQ(rowSet.ColumnCount, 4);

    std::ostringstream out;
    NQdb::TConsoleSink sink(s, out);
    sink.Write(rowSet);
    sink.Flush();

    auto result = out.str();
    EXPECT_NE(result.find("alice"), std::string::npos);
    EXPECT_NE(result.find("1.5"), std::string::npos);
    EXPECT_NE(result.find("true"), std::string::npos);

    NQdb::Release(&rowSet);

    ASSERT_FALSE(source.Next(rowSet));
}

TEST(IOTest, NullsInColumn) {
    const std::string path = "/tmp/test_io_qdb_nulls.parquet";

    arrow::Int64Builder ids;
    (void)ids.Append(10);
    (void)ids.AppendNull();
    (void)ids.Append(30);

    auto schema = arrow::schema({arrow::field("id", arrow::int64())});
    auto batch = arrow::RecordBatch::Make(schema, 3, {ids.Finish().ValueOrDie()});
    WriteParquet(path, batch);

    NQdb::TParquetSource source(path);

    TRowSet rowSet = {};
    ASSERT_TRUE(source.Next(rowSet));

    const auto& col = rowSet.Columns[0];
    ASSERT_NE(col.Mask, nullptr);
    // Arrow validity bitmap: 1 = valid, 0 = null, bit-packed within each byte
    auto isValid = [&](int row) {
        return (col.Mask[row / 8] >> (row % 8)) & 1;
    };
    EXPECT_NE(isValid(0), 0); // row 0: valid
    EXPECT_EQ(isValid(1), 0); // row 1: null
    EXPECT_NE(isValid(2), 0); // row 2: valid

    std::ostringstream out;
    NQdb::TConsoleSink sink(source.Schema(), out);
    sink.Write(rowSet);
    sink.Flush();
    EXPECT_NE(out.str().find("NULL"), std::string::npos);

    NQdb::Release(&rowSet);
}

TEST(IOTest, ParquetRowGroupRangeSource) {
    const std::string path = "/tmp/test_io_qdb_rowgroups.parquet";

    arrow::Int64Builder ids;
    arrow::Int64Builder payload;
    (void)ids.AppendValues({1, 2, 3, 4, 5});
    (void)payload.AppendValues({10, 20, 30, 40, 50});

    auto schema = arrow::schema({
        arrow::field("id", arrow::int64(), false),
        arrow::field("payload", arrow::int64(), false),
    });
    auto batch = arrow::RecordBatch::Make(schema, 5, {
        ids.Finish().ValueOrDie(),
        payload.Finish().ValueOrDie(),
    });
    WriteParquet(path, batch, 2);

    NQdb::TParquetFile file(path);
    auto source = file.MakeSource();
    auto rowGroups = source->ScanRowGroups();
    ASSERT_EQ(rowGroups.size(), 3u);
    EXPECT_EQ(rowGroups[0].RowCount, 2);
    EXPECT_EQ(rowGroups[1].RowCount, 2);
    EXPECT_EQ(rowGroups[2].RowCount, 1);

    source->RestrictColumns({"id"});
    ASSERT_EQ(source->Schema().Columns.size(), 1u);
    const std::vector<std::string> columnNames{
        "payload",
        "id",
        "payload",
    };
    auto reader = source->CompileReader(columnNames);
    const std::vector<TPhysicalRowId> rowIds{
        MakeParquetRowId(2, 0),
        MakeParquetRowId(0, 1),
        MakeParquetRowId(1, 0),
    };
    TRowSet lookupRows{};
    std::string error;
    ASSERT_TRUE(reader->ReadRows(rowIds, lookupRows, &error)) << error;
    ASSERT_EQ(lookupRows.RowCount, 3);
    ASSERT_EQ(lookupRows.ColumnCount, 3);
    const auto* outPayload =
        reinterpret_cast<const int64_t*>(lookupRows.Columns[0].Data);
    const auto* outIds =
        reinterpret_cast<const int64_t*>(lookupRows.Columns[1].Data);
    const auto* outPayloadAgain =
        reinterpret_cast<const int64_t*>(lookupRows.Columns[2].Data);
    EXPECT_EQ(std::vector<int64_t>(outPayload, outPayload + 3),
        (std::vector<int64_t>{50, 20, 30}));
    EXPECT_EQ(std::vector<int64_t>(outIds, outIds + 3),
        (std::vector<int64_t>{5, 2, 3}));
    EXPECT_EQ(std::vector<int64_t>(outPayloadAgain, outPayloadAgain + 3),
        (std::vector<int64_t>{50, 20, 30}));
    Release(&lookupRows);

    auto split = source->MakeRowGroupsSource({1});
    EXPECT_EQ(split->Stats().get(), source->Stats().get());
    TRowSet rowSet = {};
    ASSERT_TRUE(split->Next(rowSet));
    ASSERT_EQ(rowSet.RowCount, 2);
    ASSERT_EQ(rowSet.ColumnCount, 1);
    ASSERT_EQ(split->Schema().Columns.size(), 1u);
    EXPECT_EQ(split->Schema().Columns[0].Name, "id");
    const auto* values = reinterpret_cast<const int64_t*>(rowSet.Columns[0].Data);
    EXPECT_EQ(values[0], 3);
    EXPECT_EQ(values[1], 4);
    NQdb::Release(&rowSet);
    EXPECT_FALSE(split->Next(rowSet));

    source->RestrictColumns({});
    int64_t rowCount = 0;
    while (source->Next(rowSet)) {
        EXPECT_EQ(rowSet.ColumnCount, 0);
        rowCount += rowSet.RowCount;
        NQdb::Release(&rowSet);
    }
    EXPECT_EQ(rowCount, 5);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
