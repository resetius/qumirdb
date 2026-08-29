#include <qdb/io/parquet/row_reader.h>

#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#include <arrow/compute/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/column_reader.h>
#include <parquet/file_reader.h>
#include <parquet/level_conversion.h>
#include <parquet/metadata.h>
#include <parquet/schema.h>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace NQdb {

namespace {

struct TLocator {
    uint32_t RowGroup;
    int64_t RowOffset;
    size_t OriginalIndex;
};

enum class EColumnRead {
    Unsupported,
    Primitive,
    Binary,
};

struct TCompiledColumn {
    int Index;
    std::shared_ptr<arrow::Field> Field;
    const parquet::ColumnDescriptor* Descriptor = nullptr;
    parquet::internal::LevelInfo Levels;
    EColumnRead Read = EColumnRead::Unsupported;
    std::shared_ptr<arrow::DataType> StorageType;
    bool ReinterpretStorage = false;
    bool CastStorage = false;
};

arrow::MemoryPool* MemoryPoolOrDefault(arrow::MemoryPool* memoryPool) {
    return memoryPool ? memoryPool : arrow::default_memory_pool();
}

TCompiledColumn CompileColumn(
    int index,
    const std::shared_ptr<arrow::Field>& field,
    const parquet::ColumnDescriptor* descriptor)
{
    TCompiledColumn column{
        .Index = index,
        .Field = field,
        .Descriptor = descriptor,
    };
    if (!descriptor || descriptor->max_repetition_level() != 0) {
        return column;
    }
    column.Levels = parquet::internal::LevelInfo::ComputeLevelInfo(descriptor);

    const auto type = field->type()->id();
    switch (descriptor->physical_type()) {
        case parquet::Type::BOOLEAN:
            if (type == arrow::Type::BOOL) {
                column.Read = EColumnRead::Primitive;
                column.StorageType = arrow::uint8();
                column.CastStorage = true;
            }
            break;
        case parquet::Type::INT32:
            if (type == arrow::Type::INT8 ||
                type == arrow::Type::INT16 ||
                type == arrow::Type::INT32 ||
                type == arrow::Type::UINT8 ||
                type == arrow::Type::UINT16 ||
                type == arrow::Type::UINT32 ||
                type == arrow::Type::DATE32)
            {
                column.Read = EColumnRead::Primitive;
                column.StorageType = arrow::int32();
                column.ReinterpretStorage =
                    type == arrow::Type::UINT32 ||
                    type == arrow::Type::DATE32;
                column.CastStorage =
                    type != arrow::Type::INT32 &&
                    !column.ReinterpretStorage;
            }
            break;
        case parquet::Type::INT64:
            if (type == arrow::Type::INT64 ||
                type == arrow::Type::UINT64 ||
                type == arrow::Type::DATE64)
            {
                column.Read = EColumnRead::Primitive;
                column.StorageType = arrow::int64();
                column.ReinterpretStorage =
                    type == arrow::Type::UINT64 ||
                    type == arrow::Type::DATE64;
            }
            break;
        case parquet::Type::FLOAT:
            if (type == arrow::Type::FLOAT) {
                column.Read = EColumnRead::Primitive;
                column.StorageType = arrow::float32();
            }
            break;
        case parquet::Type::DOUBLE:
            if (type == arrow::Type::DOUBLE) {
                column.Read = EColumnRead::Primitive;
                column.StorageType = arrow::float64();
            }
            break;
        case parquet::Type::BYTE_ARRAY:
            if (type == arrow::Type::STRING ||
                type == arrow::Type::LARGE_STRING)
            {
                column.Read = EColumnRead::Binary;
            }
            break;
        default:
            break;
    }
    return column;
}

struct TPageSelection {
    explicit TPageSelection(std::span<const int64_t> rows)
        : Rows(rows)
    {
        DenseRowsByTarget.reserve(rows.size());
    }

    bool Skip(const parquet::DataPageStats& stats) {
        const int64_t pageRows = stats.num_rows
            ? static_cast<int64_t>(*stats.num_rows)
            : static_cast<int64_t>(stats.num_values);
        if (pageRows < 0) {
            Invalid = true;
            return false;
        }

        const int64_t pageBegin = SourceRows;
        const int64_t pageEnd = pageBegin + pageRows;
        SourceRows = pageEnd;

        while (NextRow < Rows.size() && Rows[NextRow] < pageBegin) {
            Invalid = true;
            ++NextRow;
        }
        const size_t first = NextRow;
        while (NextRow < Rows.size() && Rows[NextRow] < pageEnd) {
            DenseRowsByTarget.push_back(
                DenseRows + Rows[NextRow] - pageBegin);
            ++NextRow;
        }
        const bool keep = NextRow != first;
        if (keep) {
            DenseRows += pageRows;
        }
        return !keep;
    }

    std::span<const int64_t> Rows;
    std::vector<int64_t> DenseRowsByTarget;
    size_t NextRow = 0;
    int64_t SourceRows = 0;
    int64_t DenseRows = 0;
    bool Invalid = false;
};

arrow::Result<std::shared_ptr<arrow::Array>> ReadPageSelectedColumn(
    parquet::RowGroupReader& rowGroup,
    int64_t rowGroupRows,
    const TCompiledColumn& column,
    std::span<const int64_t> rows,
    arrow::MemoryPool* memoryPool)
{
    auto pageReader = rowGroup.GetColumnPageReader(column.Index);
    if (!pageReader) {
        return arrow::Status::Invalid(
            "parquet page lookup: cannot create page reader");
    }

    TPageSelection selection(rows);
    pageReader->set_data_page_filter(
        [&selection](const parquet::DataPageStats& stats) {
            return selection.Skip(stats);
        });

    auto recordReader = parquet::internal::RecordReader::Make(
        column.Descriptor,
        column.Levels,
        memoryPool,
        false,
        false,
        column.Field->type());
    recordReader->SetPageReader(std::move(pageReader));
    const int64_t recordsRead = recordReader->ReadRecords(rowGroupRows);

    if (selection.Invalid || selection.NextRow != rows.size() ||
        selection.SourceRows != rowGroupRows ||
        selection.DenseRowsByTarget.size() != rows.size() ||
        recordsRead != selection.DenseRows)
    {
        return arrow::Status::Invalid(
            "parquet page lookup: inconsistent page row boundaries");
    }

    std::shared_ptr<arrow::Array> retained;
    if (column.Read == EColumnRead::Binary) {
        auto* binary = dynamic_cast<parquet::internal::BinaryRecordReader*>(
            recordReader.get());
        if (!binary) {
            return arrow::Status::Invalid(
                "parquet page lookup: binary record reader is unavailable");
        }
        auto chunks = binary->GetBuilderChunks();
        if (chunks.empty()) {
            return arrow::Status::Invalid(
                "parquet page lookup: binary reader returned no chunks");
        }
        if (chunks.size() == 1) {
            retained = std::move(chunks.front());
        } else {
            ARROW_ASSIGN_OR_RAISE(
                retained, arrow::Concatenate(chunks, memoryPool));
        }
    } else {
        const int64_t valuesWritten = recordReader->values_written();
        const int64_t nullCount = recordReader->null_count();
        auto values = recordReader->ReleaseValues();
        auto validity = recordReader->ReleaseIsValid();
        if (valuesWritten != selection.DenseRows || !values) {
            return arrow::Status::Invalid(
                "parquet page lookup: inconsistent primitive value count");
        }

        auto data = arrow::ArrayData::Make(
            column.StorageType,
            valuesWritten,
            {std::move(validity), std::move(values)},
            nullCount);
        retained = arrow::MakeArray(std::move(data));

        if (column.ReinterpretStorage) {
            retained = arrow::MakeArray(arrow::ArrayData::Make(
                column.Field->type(),
                valuesWritten,
                retained->data()->buffers,
                nullCount));
        } else if (column.CastStorage) {
            ARROW_ASSIGN_OR_RAISE(
                auto casted,
                arrow::compute::Cast(
                    arrow::Datum(retained), column.Field->type()));
            retained = casted.make_array();
        }
    }

    if (retained->length() != selection.DenseRows) {
        return arrow::Status::Invalid(
            "parquet page lookup: decoded page length mismatch");
    }

    arrow::Int64Builder takeBuilder(memoryPool);
    ARROW_RETURN_NOT_OK(takeBuilder.AppendValues(selection.DenseRowsByTarget));
    ARROW_ASSIGN_OR_RAISE(auto takeIndexes, takeBuilder.Finish());
    ARROW_ASSIGN_OR_RAISE(
        auto taken,
        arrow::compute::Take(
            arrow::Datum(retained), arrow::Datum(takeIndexes)));
    return taken.make_array();
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> ReadPageSelectedRows(
    parquet::arrow::FileReader& reader,
    std::span<const TLocator> locators,
    std::span<const TCompiledColumn> columns,
    const std::shared_ptr<arrow::Schema>& outputSchema,
    arrow::MemoryPool* memoryPool)
{
    auto* parquetReader = reader.parquet_reader();
    auto metadata = parquetReader->metadata();
    std::vector<std::vector<std::shared_ptr<arrow::Array>>> chunks(
        columns.size());
    try {
        size_t groupBegin = 0;
        while (groupBegin < locators.size()) {
            size_t groupEnd = groupBegin + 1;
            while (
                groupEnd < locators.size() &&
                locators[groupEnd].RowGroup == locators[groupBegin].RowGroup)
            {
                ++groupEnd;
            }

            const int rowGroupIndex =
                static_cast<int>(locators[groupBegin].RowGroup);
            const int64_t rowGroupRows =
                metadata->RowGroup(rowGroupIndex)->num_rows();
            std::vector<int64_t> rows;
            rows.reserve(groupEnd - groupBegin);
            for (size_t i = groupBegin; i < groupEnd; ++i) {
                rows.push_back(locators[i].RowOffset);
            }

            for (size_t column = 0; column < columns.size(); ++column) {
                auto rowGroup = parquetReader->RowGroup(rowGroupIndex);
                ARROW_ASSIGN_OR_RAISE(
                    auto selected,
                    ReadPageSelectedColumn(
                        *rowGroup,
                        rowGroupRows,
                        columns[column],
                        rows,
                        memoryPool));
                chunks[column].push_back(std::move(selected));
            }
            groupBegin = groupEnd;
        }
    } catch (const std::exception& e) {
        return arrow::Status::IOError(e.what());
    }

    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(columns.size());
    for (auto& columnChunks : chunks) {
        if (columnChunks.size() == 1) {
            arrays.push_back(std::move(columnChunks.front()));
        } else {
            ARROW_ASSIGN_OR_RAISE(
                auto concatenated,
                arrow::Concatenate(columnChunks, memoryPool));
            arrays.push_back(std::move(concatenated));
        }
    }

    bool alreadyOrdered = true;
    for (size_t i = 0; i < locators.size(); ++i) {
        if (locators[i].OriginalIndex != i) {
            alreadyOrdered = false;
            break;
        }
    }
    if (!alreadyOrdered) {
        std::vector<int64_t> originalToSorted(locators.size());
        for (size_t sorted = 0; sorted < locators.size(); ++sorted) {
            originalToSorted[locators[sorted].OriginalIndex] =
                static_cast<int64_t>(sorted);
        }
        arrow::Int64Builder reorderBuilder(memoryPool);
        ARROW_RETURN_NOT_OK(reorderBuilder.AppendValues(originalToSorted));
        ARROW_ASSIGN_OR_RAISE(auto reorder, reorderBuilder.Finish());
        for (auto& array : arrays) {
            ARROW_ASSIGN_OR_RAISE(
                auto reordered,
                arrow::compute::Take(
                    arrow::Datum(array), arrow::Datum(reorder)));
            array = reordered.make_array();
        }
    }

    return arrow::RecordBatch::Make(
        outputSchema,
        static_cast<int64_t>(locators.size()),
        std::move(arrays));
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> ReadWholeRowGroups(
    parquet::arrow::FileReader& reader,
    const parquet::FileMetaData& metadata,
    std::span<const TLocator> locators,
    const std::vector<int>& columnIndexes,
    arrow::MemoryPool* memoryPool)
{
    std::vector<int> rowGroups;
    std::vector<int64_t> takePositions(locators.size());
    int64_t prefix = 0;
    size_t groupBegin = 0;
    while (groupBegin < locators.size()) {
        size_t groupEnd = groupBegin + 1;
        while (
            groupEnd < locators.size() &&
            locators[groupEnd].RowGroup == locators[groupBegin].RowGroup)
        {
            ++groupEnd;
        }

        const int rowGroup = static_cast<int>(locators[groupBegin].RowGroup);
        rowGroups.push_back(rowGroup);
        for (size_t i = groupBegin; i < groupEnd; ++i) {
            takePositions[locators[i].OriginalIndex] =
                prefix + locators[i].RowOffset;
        }
        prefix += metadata.RowGroup(rowGroup)->num_rows();
        groupBegin = groupEnd;
    }

    auto readerResult = reader.GetRecordBatchReader(rowGroups, columnIndexes);
    if (!readerResult.ok()) {
        return readerResult.status();
    }
    auto batchReader = std::move(*readerResult);
    ARROW_ASSIGN_OR_RAISE(
        auto table,
        arrow::Table::FromRecordBatchReader(batchReader.get()));

    arrow::Int64Builder takeBuilder(memoryPool);
    ARROW_RETURN_NOT_OK(takeBuilder.AppendValues(takePositions));
    ARROW_ASSIGN_OR_RAISE(auto takeIndexes, takeBuilder.Finish());
    ARROW_ASSIGN_OR_RAISE(
        auto taken,
        arrow::compute::Take(
            arrow::Datum(table), arrow::Datum(takeIndexes)));
    return taken.table()->CombineChunksToBatch(memoryPool);
}

} // namespace

struct TParquetRowReader::TImpl {
    TImpl(
        std::shared_ptr<parquet::arrow::FileReader> reader,
        std::vector<int> columnIndexes,
        arrow::MemoryPool* memoryPool)
        : MemoryPool(memoryPool)
        , Reader(std::move(reader))
        , ColumnIndexes(std::move(columnIndexes))
    {
        if (!Reader) {
            throw std::invalid_argument("parquet row reader is null");
        }
        std::shared_ptr<arrow::Schema> schema;
        auto status = Reader->GetSchema(&schema);
        if (!status.ok()) {
            throw std::runtime_error(status.ToString());
        }
        if (ColumnIndexes.empty()) {
            throw std::invalid_argument(
                "parquet row reader requires at least one column index");
        }

        Metadata = Reader->parquet_reader()->metadata();
        const bool flatSchema =
            Metadata->num_columns() == schema->num_fields();
        Columns.reserve(ColumnIndexes.size());
        std::vector<std::shared_ptr<arrow::Field>> outputFields;
        outputFields.reserve(ColumnIndexes.size());
        SupportsPageLookup = flatSchema;
        for (int index : ColumnIndexes) {
            if (index < 0 || index >= schema->num_fields()) {
                throw std::out_of_range(
                    "parquet column index is outside the file schema");
            }
            const parquet::ColumnDescriptor* descriptor = flatSchema
                ? Metadata->schema()->Column(index)
                : nullptr;
            auto column = CompileColumn(index, schema->field(index), descriptor);
            SupportsPageLookup &= column.Read != EColumnRead::Unsupported;
            outputFields.push_back(column.Field);
            Columns.push_back(std::move(column));
        }
        OutputSchema = arrow::schema(std::move(outputFields));
    }

    arrow::MemoryPool* MemoryPool;
    std::shared_ptr<parquet::arrow::FileReader> Reader;
    std::shared_ptr<parquet::FileMetaData> Metadata;
    std::vector<int> ColumnIndexes;
    std::vector<TCompiledColumn> Columns;
    std::shared_ptr<arrow::Schema> OutputSchema;
    bool SupportsPageLookup = false;
};

TParquetRowReader::TParquetRowReader(
    std::shared_ptr<parquet::arrow::FileReader> reader,
    std::vector<int> columnIndexes,
    arrow::MemoryPool* memoryPool)
    : Impl_(std::make_unique<TImpl>(
        std::move(reader),
        std::move(columnIndexes),
        MemoryPoolOrDefault(memoryPool)))
{}

TParquetRowReader::~TParquetRowReader() = default;

arrow::Result<std::shared_ptr<arrow::RecordBatch>> TParquetRowReader::ReadRows(std::span<const TPhysicalRowId> rowIds) const
{
    if (rowIds.empty()) {
        return arrow::Status::Invalid(
            "parquet row reader requires at least one row locator");
    }
    std::vector<TLocator> locators;
    locators.reserve(rowIds.size());
    for (size_t i = 0; i < rowIds.size(); ++i) {
        locators.push_back({
            .RowGroup = ParquetRowGroup(rowIds[i]),
            .RowOffset = ParquetRowOffset(rowIds[i]),
            .OriginalIndex = i,
        });
    }
    std::ranges::sort(locators, [](const TLocator& left, const TLocator& right) {
        if (left.RowGroup != right.RowGroup) {
            return left.RowGroup < right.RowGroup;
        }
        if (left.RowOffset != right.RowOffset) {
            return left.RowOffset < right.RowOffset;
        }
        return left.OriginalIndex < right.OriginalIndex;
    });

    const auto& metadata = Impl_->Metadata;
    size_t groupBegin = 0;
    while (groupBegin < locators.size()) {
        size_t groupEnd = groupBegin + 1;
        while (
            groupEnd < locators.size() &&
            locators[groupEnd].RowGroup == locators[groupBegin].RowGroup)
        {
            ++groupEnd;
        }
        const uint32_t rowGroup = locators[groupBegin].RowGroup;
        if (rowGroup >= static_cast<uint32_t>(metadata->num_row_groups())) {
            return arrow::Status::IndexError(
                "parquet row locator is outside the file");
        }
        const int64_t rowCount =
            metadata->RowGroup(static_cast<int>(rowGroup))->num_rows();
        if (locators[groupEnd - 1].RowOffset >= rowCount) {
            return arrow::Status::IndexError(
                "parquet row locator is outside the file");
        }
        groupBegin = groupEnd;
    }

    if (Impl_->SupportsPageLookup) {
        auto selected = ReadPageSelectedRows(
            *Impl_->Reader,
            locators,
            Impl_->Columns,
            Impl_->OutputSchema,
            Impl_->MemoryPool);
        if (selected.ok()) {
            return selected;
        }
        if (!selected.status().IsInvalid() &&
            !selected.status().IsNotImplemented())
        {
            return selected.status();
        }
    }
    return ReadWholeRowGroups(
        *Impl_->Reader,
        *metadata,
        locators,
        Impl_->ColumnIndexes,
        Impl_->MemoryPool);
}

} // namespace NQdb
