#include <qdb/io/parquet/source.h>
#include <qdb/plan/types/nullable.h>

#include <cstdlib>
#include <stdexcept>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/memory_pool.h>
#include <parquet/arrow/reader.h>

#include <numeric>
#include <utility>

#include <qumir/parser/type.h>

namespace NQdb {

namespace {

using NQumir::NAst::TIntegerType;
using NQumir::NAst::TTypePtr;

TTypePtr ArrowTypeToQumir(const std::shared_ptr<arrow::DataType>& type) {
    switch (type->id()) {
        case arrow::Type::INT8: return std::make_shared<TIntegerType>(TIntegerType::I8);
        case arrow::Type::INT16: return std::make_shared<TIntegerType>(TIntegerType::I16);
        case arrow::Type::INT32: return std::make_shared<TIntegerType>(TIntegerType::I32);
        case arrow::Type::INT64: return std::make_shared<TIntegerType>(TIntegerType::I64);
        case arrow::Type::UINT8: return std::make_shared<TIntegerType>(TIntegerType::U8);
        case arrow::Type::UINT16: return std::make_shared<TIntegerType>(TIntegerType::U16);
        case arrow::Type::UINT32: return std::make_shared<TIntegerType>(TIntegerType::U32);
        case arrow::Type::UINT64: return std::make_shared<TIntegerType>(TIntegerType::U64);
        case arrow::Type::FLOAT:
        case arrow::Type::DOUBLE: return std::make_shared<NQumir::NAst::TFloatType>();
        case arrow::Type::BOOL: return std::make_shared<NQumir::NAst::TBoolType>();
        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING: return std::make_shared<NQumir::NAst::TStringType>();
        case arrow::Type::DATE32: return std::make_shared<TIntegerType>(TIntegerType::I32);
        case arrow::Type::DATE64: return std::make_shared<TIntegerType>(TIntegerType::I64);
        default: return nullptr;
    }
}

TTypePtr ArrowFieldToQumir(const std::shared_ptr<arrow::Field>& field) {
    auto type = ArrowTypeToQumir(field->type());
    if (type && field->nullable()) {
        return std::make_shared<TNullable>(std::move(type));
    }
    return type;
}

struct TBatchData {
    std::shared_ptr<arrow::RecordBatch> Batch;
    std::vector<TColumn> Columns;
};

void DestroyBatch(TRowSet* rs) {
    delete static_cast<TBatchData*>(rs->Private);
}

char* NumericData(const std::shared_ptr<arrow::Array>& arr) {
    auto fwType = static_cast<const arrow::FixedWidthType*>(arr->type().get());
    int byteWidth = fwType->bit_width() / 8;
    const uint8_t* raw = arr->data()->buffers[1]->data();
    return const_cast<char*>(reinterpret_cast<const char*>(raw)) + arr->offset() * byteWidth;
}

arrow::MemoryPool* GetMemoryPool() {
    static arrow::MemoryPool* pool = []() -> arrow::MemoryPool* {
        const char* env = std::getenv("QDB_ARROW_MEMORY_POOL");
        std::string_view mode = env ? std::string_view(env) : std::string_view{};

        if (mode == "system") {
            return arrow::system_memory_pool();
        }
        if (mode == "default") {
            return arrow::default_memory_pool();
        }
        if (mode == "jemalloc") {
            arrow::MemoryPool* jemalloc = nullptr;
            if (arrow::jemalloc_memory_pool(&jemalloc).ok() && jemalloc) {
                return jemalloc;
            }
            return arrow::default_memory_pool();
        }
        if (mode == "mimalloc") {
            arrow::MemoryPool* mimalloc = nullptr;
            if (arrow::mimalloc_memory_pool(&mimalloc).ok() && mimalloc) {
                return mimalloc;
            }
            return arrow::default_memory_pool();
        }
        arrow::MemoryPool* jemalloc = nullptr;
        if (arrow::jemalloc_memory_pool(&jemalloc).ok() && jemalloc) {
            return jemalloc;
        }
        return arrow::default_memory_pool();
    }();
    return pool;
}

} // namespace

TParquetSource::TParquetSource(const std::string& path)
    : TParquetSource(path, {})
{}

TParquetSource::TParquetSource(
    const std::string& path,
    std::vector<int> rowGroups)
    : Path_(path)
    , RowGroups_(std::move(rowGroups))
{
    Open();
    ResetReader();
}

void TParquetSource::Open()
{
    auto infileResult = arrow::io::ReadableFile::Open(Path_);
    if (!infileResult.ok()) {
        throw std::runtime_error(infileResult.status().ToString());
    }
    auto infile = infileResult.ValueOrDie();

    parquet::ArrowReaderProperties arrowProps;
    arrowProps.set_batch_size(1 << 18);
    parquet::arrow::FileReaderBuilder builder;
    auto status = builder
        .memory_pool(GetMemoryPool())
        ->properties(arrowProps)
        ->Open(infile);
    if (!status.ok()) {
        throw std::runtime_error(status.ToString());
    }
    auto fileReaderResult = builder.Build();
    if (!fileReaderResult.ok()) {
        throw std::runtime_error(fileReaderResult.status().ToString());
    }
    FileReader_ = std::move(*fileReaderResult);

    std::shared_ptr<arrow::Schema> fileSchema;
    auto schemaStatus = FileReader_->GetSchema(&fileSchema);
    if (!schemaStatus.ok()) {
        throw std::runtime_error(schemaStatus.ToString());
    }
    FileNames_.clear();
    FileNames_.reserve(fileSchema->num_fields());
    for (const auto& field : fileSchema->fields()) {
        FileNames_.push_back(field->name());
    }
}

std::vector<int> TParquetSource::EffectiveRowGroups() const
{
    if (!RowGroups_.empty()) {
        return RowGroups_;
    }

    int numRgs = FileReader_->parquet_reader()->metadata()->num_row_groups();
    std::vector<int> rowGroups(numRgs);
    std::iota(rowGroups.begin(), rowGroups.end(), 0);
    return rowGroups;
}

void TParquetSource::ResetReader()
{
    auto rowGroups = EffectiveRowGroups();
    auto indices = EffectiveColumnIndices();
    auto batchReaderResult = indices.empty()
        ? FileReader_->GetRecordBatchReader(rowGroups)
        : FileReader_->GetRecordBatchReader(rowGroups, indices);
    if (!batchReaderResult.ok()) {
        throw std::runtime_error(batchReaderResult.status().ToString());
    }
    Reader_ = std::move(*batchReaderResult);

    RefreshSchema();
}

std::vector<int> TParquetSource::EffectiveColumnIndices() const
{
    if (RestrictedColumns_.empty()) {
        return {};
    }

    // Map restricted names to indices into the full file schema (FileNames_),
    // never the possibly-narrowed Names_, so repeated restrictions stay stable.
    std::vector<int> indices;
    for (int i = 0; i < static_cast<int>(FileNames_.size()); ++i) {
        if (RestrictedColumns_.count(FileNames_[i])) {
            indices.push_back(i);
        }
    }
    return indices;
}

void TParquetSource::RefreshSchema()
{
    int numFields = Reader_->schema()->num_fields();
    Names_.clear();
    Columns_.clear();
    Names_.reserve(numFields);
    Columns_.reserve(numFields);
    for (const auto& field : Reader_->schema()->fields()) {
        Names_.push_back(field->name());
        Columns_.push_back({
            .Name = Names_.back(),
            .Type = ArrowFieldToQumir(field),
        });
    }
    Schema_ = TSchema{std::span<const TColumnSchema>(Columns_)};
}

TParquetSource::~TParquetSource() = default;

void TParquetSource::RestrictColumns(const std::unordered_set<std::string>& names) {
    if (names.empty()) {
        return;
    }

    RestrictedColumns_ = names;
    ResetReader();
}

std::vector<NScheduler::TScanRowGroup> TParquetSource::ScanRowGroups() const
{
    auto metadata = FileReader_->parquet_reader()->metadata();
    std::vector<NScheduler::TScanRowGroup> rowGroups;
    rowGroups.reserve(static_cast<size_t>(metadata->num_row_groups()));
    for (int i = 0; i < metadata->num_row_groups(); ++i) {
        auto rowGroup = metadata->RowGroup(i);
        rowGroups.push_back({
            .RowGroup = static_cast<size_t>(i),
            .RowCount = rowGroup->num_rows(),
            .ByteSize = rowGroup->total_byte_size(),
        });
    }
    return rowGroups;
}

std::unique_ptr<TParquetSource> TParquetSource::MakeRowGroupRangeSource(
    size_t firstRowGroup,
    size_t rowGroupCount) const
{
    std::vector<int> rowGroups;
    rowGroups.reserve(rowGroupCount);
    for (size_t i = 0; i < rowGroupCount; ++i) {
        rowGroups.push_back(static_cast<int>(firstRowGroup + i));
    }
    auto source = std::unique_ptr<TParquetSource>(
        new TParquetSource(Path_, std::move(rowGroups)));
    if (!RestrictedColumns_.empty()) {
        source->RestrictColumns(RestrictedColumns_);
    }
    return source;
}

const TSchema& TParquetSource::Schema() const {
    return Schema_;
}

bool TParquetSource::Next(TRowSet& rowSet) {
    std::shared_ptr<arrow::RecordBatch> batch;
    if (!Reader_->ReadNext(&batch).ok() || !batch) {
        return false;
    }

    int32_t numCols = static_cast<int32_t>(batch->num_columns());
    int64_t len = batch->num_rows();

    auto* data = new TBatchData;
    data->Batch = batch;
    data->Columns.resize(numCols);

    for (int32_t i = 0; i < numCols; ++i) {
        auto arr = batch->column(i);
        TColumn& col = data->Columns[i];
        col.Offsets = nullptr;
        col.OffsetWidth = 0;
        col.DataBitOffset = 0;
        col.Mask = nullptr;
        col.MaskBitOffset = 0;

        if (arr->null_count() > 0) {
            col.Mask = arr->null_bitmap_data();
            col.MaskBitOffset = static_cast<int32_t>(arr->offset());
        }

        switch (arr->type_id()) {
            case arrow::Type::INT8:
            case arrow::Type::INT16:
            case arrow::Type::INT32:
            case arrow::Type::INT64:
            case arrow::Type::UINT8:
            case arrow::Type::UINT16:
            case arrow::Type::UINT32:
            case arrow::Type::UINT64:
            case arrow::Type::DOUBLE:
            case arrow::Type::DATE32:
            case arrow::Type::DATE64: {
                col.Data = NumericData(arr);
                break;
            }
            case arrow::Type::FLOAT: {
                col.Data = NumericData(arr);
                break;
            }
            case arrow::Type::BOOL: {
                auto bitmap = arr->data()->buffers[1];
                col.Data = const_cast<char*>(reinterpret_cast<const char*>(bitmap->data()));
                col.DataBitOffset = static_cast<int32_t>(arr->offset());
                break;
            }
            case arrow::Type::STRING: {
                auto typed = std::static_pointer_cast<arrow::StringArray>(arr);
                const int32_t* src = typed->raw_value_offsets() + arr->offset();
                int32_t base = src[0];
                col.Offsets = const_cast<int32_t*>(src);
                col.OffsetWidth = 4;
                col.Data = const_cast<char*>(reinterpret_cast<const char*>(typed->raw_data()) + base);
                break;
            }
            case arrow::Type::LARGE_STRING: {
                auto typed = std::static_pointer_cast<arrow::LargeStringArray>(arr);
                const int64_t* src = typed->raw_value_offsets() + arr->offset();
                int64_t base = src[0];
                col.Offsets = const_cast<int64_t*>(src);
                col.OffsetWidth = 8;
                col.Data = const_cast<char*>(reinterpret_cast<const char*>(typed->raw_data()) + base);
                break;
            }
            default: {
                col.Data = nullptr;
                break;
            }
        }
    }

    rowSet = {
        .Columns = data->Columns.data(),
        .ColumnCount = numCols,
        .RowCount = static_cast<int32_t>(len),
        .Selection = nullptr,
        .Destroy = DestroyBatch,
        .Private = data,
        .RefCount = 1,
    };

    return true;
}

} // namespace NQdb
