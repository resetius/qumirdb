#include <qdb/io/parquet/source.h>

#include <stdexcept>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>

#include <qumir/parser/type.h>

namespace NQqb {

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
        default: return nullptr;
    }
}

struct TBatchData {
    std::shared_ptr<arrow::RecordBatch> Batch;
    std::vector<TColumn> Columns;
    std::vector<std::vector<char>> Masks;
    std::vector<std::vector<char>> BoolData;
    std::vector<std::vector<int64_t>> Offsets;
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

} // namespace

TParquetSource::TParquetSource(const std::string& path) {
    auto infile = arrow::io::ReadableFile::Open(path).ValueOrDie();

    auto fileReaderResult = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
    if (!fileReaderResult.ok()) {
        throw std::runtime_error(fileReaderResult.status().ToString());
    }
    FileReader_ = std::move(*fileReaderResult);

    auto batchReaderResult = FileReader_->GetRecordBatchReader();
    if (!batchReaderResult.ok()) {
        throw std::runtime_error(batchReaderResult.status().ToString());
    }
    Reader_ = std::move(*batchReaderResult);

    int numFields = Reader_->schema()->num_fields();
    Names_.reserve(numFields);
    Columns_.reserve(numFields);
    for (const auto& field : Reader_->schema()->fields()) {
        Names_.push_back(field->name());
        Columns_.push_back({
            .Name = Names_.back(),
            .Type = ArrowTypeToQumir(field->type()),
        });
    }
    Schema_ = TSchema{std::span<const TColumnSchema>(Columns_)};
}

TParquetSource::~TParquetSource() = default;

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
    data->Masks.resize(numCols);
    data->BoolData.resize(numCols);
    data->Offsets.resize(numCols);

    for (int32_t i = 0; i < numCols; ++i) {
        auto arr = batch->column(i);
        TColumn& col = data->Columns[i];

        if (arr->null_count() > 0) {
            int64_t offset = arr->offset();
            const uint8_t* bits = arr->null_bitmap_data();
            data->Masks[i].resize(len);
            for (int64_t j = 0; j < len; ++j) {
                int64_t pos = offset + j;
                uint8_t valid = (bits[pos >> 3] >> (pos & 7)) & 1;
                data->Masks[i][j] = valid ? 0 : 1;
            }
            col.Mask = data->Masks[i].data();
        } else {
            col.Mask = nullptr;
        }

        col.Offsets = nullptr;

        switch (arr->type_id()) {
            case arrow::Type::INT8:
            case arrow::Type::INT16:
            case arrow::Type::INT32:
            case arrow::Type::INT64:
            case arrow::Type::UINT8:
            case arrow::Type::UINT16:
            case arrow::Type::UINT32:
            case arrow::Type::UINT64:
            case arrow::Type::DOUBLE: {
                col.Data = NumericData(arr);
                break;
            }
            case arrow::Type::FLOAT: {
                col.Data = NumericData(arr);
                break;
            }
            case arrow::Type::BOOL: {
                auto typed = std::static_pointer_cast<arrow::BooleanArray>(arr);
                data->BoolData[i].resize(len);
                for (int64_t j = 0; j < len; ++j) {
                    data->BoolData[i][j] = typed->Value(j) ? 1 : 0;
                }
                col.Data = data->BoolData[i].data();
                break;
            }
            case arrow::Type::STRING: {
                auto typed = std::static_pointer_cast<arrow::StringArray>(arr);
                const int32_t* src = typed->raw_value_offsets();
                int32_t base = src[0];
                data->Offsets[i].resize(len + 1);
                for (int64_t j = 0; j <= len; ++j) {
                    data->Offsets[i][j] = src[j] - base;
                }
                col.Offsets = data->Offsets[i].data();
                col.Data = const_cast<char*>(reinterpret_cast<const char*>(typed->raw_data()) + base);
                break;
            }
            case arrow::Type::LARGE_STRING: {
                auto typed = std::static_pointer_cast<arrow::LargeStringArray>(arr);
                const int64_t* src = typed->raw_value_offsets();
                int64_t base = src[0];
                data->Offsets[i].resize(len + 1);
                for (int64_t j = 0; j <= len; ++j) {
                    data->Offsets[i][j] = src[j] - base;
                }
                col.Offsets = data->Offsets[i].data();
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

} // namespace NQqb
