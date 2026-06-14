#include <gtest/gtest.h>

#include <qdb/io/parquet/source.h>
#include <qdb/io/text/sink.h>
#include <qdb/ops/source.h>
#include <qdb/types/nullable.h>

#include <sstream>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

using namespace NQqb;

namespace {

void WriteParquet(
    const std::string& path,
    std::shared_ptr<arrow::RecordBatch> batch)
{
    auto outfile = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
    auto status = parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), outfile, /*chunk_size=*/1024);
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

    NQqb::TParquetSource source(path);
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
    NQqb::TConsoleSink sink(s, out);
    sink.Write(rowSet);
    sink.Flush();

    auto result = out.str();
    EXPECT_NE(result.find("alice"), std::string::npos);
    EXPECT_NE(result.find("1.5"), std::string::npos);
    EXPECT_NE(result.find("true"), std::string::npos);

    NQqb::Release(&rowSet);

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

    NQqb::TParquetSource source(path);

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
    NQqb::TConsoleSink sink(source.Schema(), out);
    sink.Write(rowSet);
    sink.Flush();
    EXPECT_NE(out.str().find("NULL"), std::string::npos);

    NQqb::Release(&rowSet);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
