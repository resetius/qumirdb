#include <gtest/gtest.h>

#include <qdb/io/parquet/row_reader.h>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace NQdb;

namespace {

void WriteParquet(
    const std::string& path,
    const std::shared_ptr<arrow::RecordBatch>& batch,
    int64_t rowGroupSize)
{
    auto output = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    auto table = arrow::Table::FromRecordBatches({batch}).ValueOrDie();
    parquet::WriterProperties::Builder properties;
    properties
        .max_rows_per_page(16)
        ->data_pagesize(256)
        ->disable_write_page_index();
    auto status = parquet::arrow::WriteTable(
        *table,
        arrow::default_memory_pool(),
        output,
        rowGroupSize,
        properties.build());
    ASSERT_TRUE(status.ok()) << status.ToString();
}

std::shared_ptr<parquet::arrow::FileReader> OpenParquet(
    const std::string& path)
{
    auto input = arrow::io::ReadableFile::Open(path).ValueOrDie();
    parquet::arrow::FileReaderBuilder builder;
    auto status = builder.Open(input);
    if (!status.ok()) {
        throw std::runtime_error(status.ToString());
    }
    auto reader = builder.Build().ValueOrDie();
    return std::shared_ptr<parquet::arrow::FileReader>(std::move(reader));
}

} // namespace

TEST(ParquetRowReaderTest, LocatorUsesHighAndLow32Bits) {
    const auto locator = MakeParquetRowId(UINT32_MAX, UINT32_MAX);
    EXPECT_EQ(ParquetRowGroup(locator), UINT32_MAX);
    EXPECT_EQ(ParquetRowOffset(locator), UINT32_MAX);
    EXPECT_THROW(
        MakeParquetRowId(uint64_t{UINT32_MAX} + 1, 0),
        std::out_of_range);
    EXPECT_THROW(
        MakeParquetRowId(0, uint64_t{UINT32_MAX} + 1),
        std::out_of_range);
}

TEST(ParquetRowReaderTest, ReadsSelectedPagesInLocatorOrder) {
    const std::string path = "/tmp/test_parquet_row_reader_qdb.parquet";
    constexpr int64_t rowGroupSize = 256;
    constexpr int64_t rowCount = rowGroupSize * 2;

    arrow::Int64Builder integers;
    arrow::StringBuilder strings;
    arrow::BooleanBuilder booleans;
    arrow::BinaryBuilder binary;
    for (int64_t row = 0; row < rowCount; ++row) {
        if (row % 7 == 0) {
            ASSERT_TRUE(integers.AppendNull().ok());
        } else {
            ASSERT_TRUE(integers.Append(row * 10).ok());
        }
        if (row % 11 == 0) {
            ASSERT_TRUE(strings.AppendNull().ok());
        } else {
            ASSERT_TRUE(strings.Append("value-" + std::to_string(row)).ok());
        }
        if (row % 5 == 0) {
            ASSERT_TRUE(booleans.AppendNull().ok());
        } else {
            ASSERT_TRUE(booleans.Append((row & 1) != 0).ok());
        }
        ASSERT_TRUE(binary.Append("raw").ok());
    }

    auto schema = arrow::schema({
        arrow::field("i", arrow::int64(), true),
        arrow::field("s", arrow::utf8(), true),
        arrow::field("b", arrow::boolean(), true),
        arrow::field("raw", arrow::binary(), false),
    });
    auto batch = arrow::RecordBatch::Make(schema, rowCount, {
        integers.Finish().ValueOrDie(),
        strings.Finish().ValueOrDie(),
        booleans.Finish().ValueOrDie(),
        binary.Finish().ValueOrDie(),
    });
    WriteParquet(path, batch, rowGroupSize);

    const std::vector<TPhysicalRowId> locators{
        MakeParquetRowId(1, 200),
        MakeParquetRowId(0, 7),
        MakeParquetRowId(1, 22),
        MakeParquetRowId(0, 3),
        MakeParquetRowId(1, 255),
        MakeParquetRowId(0, 3),
        MakeParquetRowId(0, 127),
    };
    const std::vector<std::string> columnNames{"s", "i", "b"};
    auto fileReader = OpenParquet(path);
    TParquetRowReader reader(fileReader, columnNames);
    auto result = reader.ReadRows(locators);
    ASSERT_TRUE(result.ok()) << result.status().ToString();

    const auto& output = *result;
    ASSERT_EQ(output->num_rows(), static_cast<int64_t>(locators.size()));
    ASSERT_EQ(output->num_columns(), 3);
    auto outputStrings =
        std::static_pointer_cast<arrow::StringArray>(output->column(0));
    auto outputIntegers =
        std::static_pointer_cast<arrow::Int64Array>(output->column(1));
    auto outputBooleans =
        std::static_pointer_cast<arrow::BooleanArray>(output->column(2));

    for (size_t i = 0; i < locators.size(); ++i) {
        const int64_t row =
            static_cast<int64_t>(ParquetRowGroup(locators[i])) * rowGroupSize +
            ParquetRowOffset(locators[i]);
        EXPECT_EQ(outputStrings->IsValid(static_cast<int64_t>(i)), row % 11 != 0);
        if (row % 11 != 0) {
            EXPECT_EQ(
                outputStrings->GetString(static_cast<int64_t>(i)),
                "value-" + std::to_string(row));
        }
        EXPECT_EQ(outputIntegers->IsValid(static_cast<int64_t>(i)), row % 7 != 0);
        if (row % 7 != 0) {
            EXPECT_EQ(outputIntegers->Value(static_cast<int64_t>(i)), row * 10);
        }
        EXPECT_EQ(outputBooleans->IsValid(static_cast<int64_t>(i)), row % 5 != 0);
        if (row % 5 != 0) {
            EXPECT_EQ(
                outputBooleans->Value(static_cast<int64_t>(i)),
                (row & 1) != 0);
        }
    }

    auto badLocator = reader.ReadRows(
        std::vector<TPhysicalRowId>{MakeParquetRowId(2, 0)});
    EXPECT_FALSE(badLocator.ok());

    const std::vector<std::string> fallbackColumnNames{"i", "raw", "i"};
    TParquetRowReader fallbackReader(fileReader, fallbackColumnNames);
    auto fallbackResult = fallbackReader.ReadRows(locators);
    ASSERT_TRUE(fallbackResult.ok()) << fallbackResult.status().ToString();
    const auto& fallbackOutput = *fallbackResult;
    ASSERT_EQ(fallbackOutput->num_rows(),
        static_cast<int64_t>(locators.size()));
    ASSERT_EQ(fallbackOutput->num_columns(), 3);
    EXPECT_EQ(fallbackOutput->schema()->field(0)->name(), "i");
    EXPECT_EQ(fallbackOutput->schema()->field(1)->name(), "raw");
    EXPECT_EQ(fallbackOutput->schema()->field(2)->name(), "i");
    EXPECT_TRUE(fallbackOutput->column(0)->Equals(
        fallbackOutput->column(2)));

    EXPECT_THROW(
        TParquetRowReader(fileReader, std::vector<std::string>{"missing"}),
        std::invalid_argument);
}

TEST(ParquetRowReaderTest, RejectsNestedFileSchema) {
    const std::string path = "/tmp/test_parquet_row_reader_nested_qdb.parquet";

    arrow::Int64Builder values;
    ASSERT_TRUE(values.AppendValues({10, 20}).ok());
    auto valueArray = values.Finish().ValueOrDie();
    auto valueField = arrow::field("value", arrow::int64(), false);
    auto nested = arrow::StructArray::Make(
        {valueArray}, {valueField}).ValueOrDie();
    auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("nested", nested->type(), false)}),
        2,
        {nested});
    WriteParquet(path, batch, 2);

    EXPECT_THROW(
        TParquetRowReader(
            OpenParquet(path), std::vector<std::string>{"nested"}),
        std::invalid_argument);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
