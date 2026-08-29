#include <qdb/io/parquet/source.h>
#include <qdb/io/parquet/row_reader.h>
#include <qdb/io/parquet/row_group_predicate.h>
#include <qdb/plan/types/decimal.h>
#include <qdb/plan/types/nullable.h>

#include <cstdlib>
#include <stdexcept>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/memory_pool.h>
#include <arrow/util/key_value_metadata.h>
#include <parquet/arrow/reader.h>
#include <parquet/metadata.h>
#include <parquet/schema.h>
#include <parquet/statistics.h>

#include <llvm/Support/JSON.h>

#include <bit>
#include <cmath>
#include <numeric>
#include <unordered_map>
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
        case arrow::Type::DECIMAL128: {
            auto decimal = std::static_pointer_cast<arrow::Decimal128Type>(type);
            return std::make_shared<TDecimal>(decimal->precision(), decimal->scale());
        }
        case arrow::Type::DECIMAL256:
            throw std::runtime_error("DECIMAL256 parquet columns are not supported");
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

char* FixedByteWidthData(const std::shared_ptr<arrow::Array>& arr, int32_t byteWidth) {
    const uint8_t* raw = arr->data()->buffers[1]->data();
    return const_cast<char*>(reinterpret_cast<const char*>(raw)) + arr->offset() * byteWidth;
}

void RecordBatchToRowSet(
    std::shared_ptr<arrow::RecordBatch> batch,
    TRowSet& rowSet)
{
    int32_t numCols = static_cast<int32_t>(batch->num_columns());
    int64_t len = batch->num_rows();

    auto* data = new TBatchData;
    data->Batch = std::move(batch);
    data->Columns.resize(numCols);

    for (int32_t i = 0; i < numCols; ++i) {
        auto arr = data->Batch->column(i);
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
            case arrow::Type::DECIMAL128: {
                col.Data = FixedByteWidthData(arr, 16);
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
        .RowCount = len,
        .Selection = nullptr,
        .Destroy = DestroyBatch,
        .Private = data,
        .RefCount = 1,
    };
}

class TParquetPhysicalRowReader final : public IPhysicalRowReader {
public:
    TParquetPhysicalRowReader(
        std::shared_ptr<parquet::arrow::FileReader> reader,
        std::span<const std::string> columnNames,
        arrow::MemoryPool* memoryPool)
        : Reader_(std::move(reader), columnNames, memoryPool)
    {}

    bool ReadRows(
        std::span<const TPhysicalRowId> rowIds,
        TRowSet& output,
        std::string* error) const override
    {
        auto batch = Reader_.ReadRows(rowIds);
        if (!batch.ok()) {
            if (error) {
                *error = batch.status().ToString();
            }
            return false;
        }
        RecordBatchToRowSet(std::move(*batch), output);
        return true;
    }

private:
    TParquetRowReader Reader_;
};

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

struct TParquetFileData {
    explicit TParquetFileData(const std::string& path)
        : Path(path)
    {
        Open();
    }

    void Open();
    void LoadColumnStats();
    void LoadStandardColumnStats(TStats& stats);
    void LoadRowGroupRanges();

    std::string Path;
    std::shared_ptr<parquet::arrow::FileReader> FileReader;
    std::shared_ptr<arrow::Schema> FileSchema;
    std::vector<std::string> FileNames;
    std::vector<TColumnSchema> FileColumns;
    TSchema FullSchema;
    std::vector<NScheduler::TScanRowGroup> RowGroups;
    std::vector<std::vector<TRowGroupColumnRange>> RowGroupRanges;
    TStatsPtr Stats;
};

void TParquetFileData::Open()
{
    auto infileResult = arrow::io::ReadableFile::Open(Path);
    if (!infileResult.ok()) {
        throw std::runtime_error(infileResult.status().ToString());
    }
    auto infile = infileResult.ValueOrDie();

    parquet::ArrowReaderProperties arrowProps;
    arrowProps.set_batch_size(1 << 18);
    // QDB schedules reads itself; Arrow pre-buffering would perform data I/O
    // while constructing the logical source, before scan pruning.
    arrowProps.set_pre_buffer(false);
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
    FileReader = std::move(*fileReaderResult);

    auto schemaStatus = FileReader->GetSchema(&FileSchema);
    if (!schemaStatus.ok()) {
        throw std::runtime_error(schemaStatus.ToString());
    }
    FileNames.reserve(FileSchema->num_fields());
    for (const auto& field : FileSchema->fields()) {
        FileNames.push_back(field->name());
    }
    FileColumns.reserve(FileSchema->num_fields());
    for (int field = 0; field < FileSchema->num_fields(); ++field) {
        FileColumns.push_back({
            .Name = FileNames[static_cast<size_t>(field)],
            .Type = ArrowFieldToQumir(FileSchema->field(field)),
        });
    }
    FullSchema = TSchema{FileColumns};

    LoadColumnStats();
    LoadRowGroupRanges();

    auto metadata = FileReader->parquet_reader()->metadata();
    RowGroups.reserve(static_cast<size_t>(metadata->num_row_groups()));
    for (int i = 0; i < metadata->num_row_groups(); ++i) {
        auto rowGroup = metadata->RowGroup(i);
        RowGroups.push_back({
            .RowGroup = static_cast<size_t>(i),
            .RowCount = rowGroup->num_rows(),
            .ByteSize = rowGroup->total_byte_size(),
        });
    }
}

void TParquetFileData::LoadRowGroupRanges()
{
    auto metadata = FileReader->parquet_reader()->metadata();
    const int numRowGroups = metadata->num_row_groups();
    RowGroupRanges.resize(static_cast<size_t>(numRowGroups));

    std::unordered_map<std::string_view, size_t> fieldByName;
    fieldByName.reserve(FileNames.size());
    for (size_t field = 0; field < FileNames.size(); ++field) {
        fieldByName.emplace(FileNames[field], field);
    }

    for (int rg = 0; rg < numRowGroups; ++rg) {
        const auto rowGroup = metadata->RowGroup(rg);
        auto& ranges = RowGroupRanges[static_cast<size_t>(rg)];
        ranges.resize(FileNames.size());
        for (size_t field = 0; field < FileNames.size(); ++field) {
            ranges[field].MayBeNull = FileSchema->field(static_cast<int>(field))->nullable();
            ranges[field].MayBeValue = rowGroup->num_rows() > 0;
        }

        for (int column = 0; column < metadata->num_columns(); ++column) {
            const auto* descriptor = metadata->schema()->Column(column);
            auto found = fieldByName.find(descriptor->name());
            if (found == fieldByName.end()) {
                continue; // nested/unsupported schema shape
            }
            const size_t field = found->second;
            auto& range = ranges[field];
            const auto typeId = FileSchema->field(static_cast<int>(field))->type()->id();
            range.MayBeNaN =
                typeId == arrow::Type::FLOAT || typeId == arrow::Type::DOUBLE;

            auto chunk = rowGroup->ColumnChunk(column);
            if (!chunk->is_stats_set()) {
                continue;
            }
            auto stats = chunk->statistics();
            if (!stats) {
                continue;
            }
            if (stats->HasNullCount()) {
                const int64_t nulls = stats->null_count();
                range.MayBeNull = nulls > 0;
                range.MayBeValue = nulls < rowGroup->num_rows();
            }
            if (!range.MayBeValue || !stats->HasMinMax()) {
                continue;
            }

            switch (typeId) {
                case arrow::Type::INT8:
                case arrow::Type::INT16:
                case arrow::Type::INT32:
                case arrow::Type::DATE32: {
                    if (descriptor->physical_type() != parquet::Type::INT32) {
                        break;
                    }
                    auto typed = std::static_pointer_cast<parquet::Int32Statistics>(stats);
                    const int64_t lo = typed->min();
                    const int64_t hi = typed->max();
                    if (lo <= hi) {
                        range.MinValue = std::bit_cast<uint64_t>(lo);
                        range.MaxValue = std::bit_cast<uint64_t>(hi);
                        range.HasBounds = true;
                    }
                    break;
                }
                case arrow::Type::INT64:
                case arrow::Type::DATE64: {
                    if (descriptor->physical_type() != parquet::Type::INT64) {
                        break;
                    }
                    auto typed = std::static_pointer_cast<parquet::Int64Statistics>(stats);
                    const int64_t lo = typed->min();
                    const int64_t hi = typed->max();
                    if (lo <= hi) {
                        range.MinValue = std::bit_cast<uint64_t>(lo);
                        range.MaxValue = std::bit_cast<uint64_t>(hi);
                        range.HasBounds = true;
                    }
                    break;
                }
                case arrow::Type::UINT8:
                case arrow::Type::UINT16:
                case arrow::Type::UINT32: {
                    if (descriptor->physical_type() != parquet::Type::INT32) {
                        break;
                    }
                    auto typed = std::static_pointer_cast<parquet::Int32Statistics>(stats);
                    const uint64_t lo = static_cast<uint32_t>(typed->min());
                    const uint64_t hi = static_cast<uint32_t>(typed->max());
                    if (lo <= hi) {
                        range.MinValue = lo;
                        range.MaxValue = hi;
                        range.HasBounds = true;
                    }
                    break;
                }
                case arrow::Type::UINT64: {
                    if (descriptor->physical_type() != parquet::Type::INT64) {
                        break;
                    }
                    auto typed = std::static_pointer_cast<parquet::Int64Statistics>(stats);
                    const uint64_t lo = static_cast<uint64_t>(typed->min());
                    const uint64_t hi = static_cast<uint64_t>(typed->max());
                    if (lo <= hi) {
                        range.MinValue = lo;
                        range.MaxValue = hi;
                        range.HasBounds = true;
                    }
                    break;
                }
                case arrow::Type::FLOAT: {
                    if (descriptor->physical_type() != parquet::Type::FLOAT) {
                        break;
                    }
                    auto typed = std::static_pointer_cast<parquet::FloatStatistics>(stats);
                    const double lo = typed->min();
                    const double hi = typed->max();
                    if (!std::isnan(lo) && !std::isnan(hi) && lo <= hi) {
                        range.MinValue = std::bit_cast<uint64_t>(lo);
                        range.MaxValue = std::bit_cast<uint64_t>(hi);
                        range.HasBounds = true;
                    }
                    break;
                }
                case arrow::Type::DOUBLE: {
                    if (descriptor->physical_type() != parquet::Type::DOUBLE) {
                        break;
                    }
                    auto typed = std::static_pointer_cast<parquet::DoubleStatistics>(stats);
                    const double lo = typed->min();
                    const double hi = typed->max();
                    if (!std::isnan(lo) && !std::isnan(hi) && lo <= hi) {
                        range.MinValue = std::bit_cast<uint64_t>(lo);
                        range.MaxValue = std::bit_cast<uint64_t>(hi);
                        range.HasBounds = true;
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}

TParquetFile::TParquetFile(const std::string& path)
    : Data_(std::make_shared<TParquetFileData>(path))
{}

TParquetFile::~TParquetFile() = default;

std::unique_ptr<TParquetSource> TParquetFile::MakeSource() const
{
    return std::unique_ptr<TParquetSource>(
        new TParquetSource(Data_, std::nullopt, std::nullopt));
}

TParquetSource::TParquetSource(const std::string& path)
    : TParquetSource(
        std::make_shared<TParquetFileData>(path), std::nullopt, std::nullopt)
{}

TParquetSource::TParquetSource(
    std::shared_ptr<TParquetFileData> file,
    std::optional<std::vector<int>> rowGroups,
    std::optional<std::unordered_set<std::string>> restrictedColumns)
    : File_(std::move(file))
    , RowGroups_(std::move(rowGroups))
    , RestrictedColumns_(std::move(restrictedColumns))
{
    ResetReader();
}

std::vector<int> TParquetSource::EffectiveRowGroups() const
{
    if (RowGroups_) {
        return *RowGroups_;
    }

    const int numRgs = static_cast<int>(File_->RowGroups.size());
    std::vector<int> rowGroups(numRgs);
    std::iota(rowGroups.begin(), rowGroups.end(), 0);
    return rowGroups;
}

void TParquetSource::ResetReader()
{
    auto rowGroups = EffectiveRowGroups();
    auto fileColumnIndexes = EffectiveFileColumnIndexes();
    auto batchReaderResult = !RestrictedColumns_
        ? File_->FileReader->GetRecordBatchReader(rowGroups)
        : File_->FileReader->GetRecordBatchReader(
            rowGroups, fileColumnIndexes);
    if (!batchReaderResult.ok()) {
        throw std::runtime_error(batchReaderResult.status().ToString());
    }
    Reader_ = std::move(*batchReaderResult);

    RefreshSchema();
}

std::vector<int> TParquetSource::EffectiveFileColumnIndexes() const
{
    if (!RestrictedColumns_) {
        return {};
    }

    // Map restricted names to indices into the full file schema,
    // never the possibly-narrowed Names_, so repeated restrictions stay stable.
    std::vector<int> indices;
    for (int i = 0; i < static_cast<int>(File_->FileNames.size()); ++i) {
        if (RestrictedColumns_->count(File_->FileNames[i])) {
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

void TParquetFileData::LoadStandardColumnStats(TStats& stats)
{
    auto metadata = FileReader->parquet_reader()->metadata();
    const int numColumns = metadata->num_columns();
    const int numRowGroups = metadata->num_row_groups();
    const parquet::SchemaDescriptor* schema = metadata->schema();

    for (int c = 0; c < numColumns; ++c) {
        const parquet::ColumnDescriptor* descr = schema->Column(c);
        const parquet::Type::type physType = descr->physical_type();
        const bool isFloat =
            physType == parquet::Type::FLOAT || physType == parquet::Type::DOUBLE;

        int64_t nullCount = 0;
        bool haveNull = false;
        bool haveMinMax = false;
        double minD = 0.0;
        double maxD = 0.0;
        int64_t minI = 0;
        int64_t maxI = 0;

        for (int rg = 0; rg < numRowGroups; ++rg) {
            auto colChunk = metadata->RowGroup(rg)->ColumnChunk(c);
            if (!colChunk->is_stats_set()) {
                continue;
            }
            auto st = colChunk->statistics();
            if (!st) {
                continue;
            }
            if (st->HasNullCount()) {
                nullCount += st->null_count();
                haveNull = true;
            }
            if (!st->HasMinMax()) {
                continue;
            }
            switch (physType) {
                case parquet::Type::INT32: {
                    auto t = std::static_pointer_cast<parquet::Int32Statistics>(st);
                    const int64_t lo = t->min();
                    const int64_t hi = t->max();
                    if (!haveMinMax || lo < minI) minI = lo;
                    if (!haveMinMax || hi > maxI) maxI = hi;
                    haveMinMax = true;
                    break;
                }
                case parquet::Type::INT64: {
                    auto t = std::static_pointer_cast<parquet::Int64Statistics>(st);
                    const int64_t lo = t->min();
                    const int64_t hi = t->max();
                    if (!haveMinMax || lo < minI) minI = lo;
                    if (!haveMinMax || hi > maxI) maxI = hi;
                    haveMinMax = true;
                    break;
                }
                case parquet::Type::FLOAT: {
                    auto t = std::static_pointer_cast<parquet::FloatStatistics>(st);
                    const double lo = t->min();
                    const double hi = t->max();
                    if (!haveMinMax || lo < minD) minD = lo;
                    if (!haveMinMax || hi > maxD) maxD = hi;
                    haveMinMax = true;
                    break;
                }
                case parquet::Type::DOUBLE: {
                    auto t = std::static_pointer_cast<parquet::DoubleStatistics>(st);
                    const double lo = t->min();
                    const double hi = t->max();
                    if (!haveMinMax || lo < minD) minD = lo;
                    if (!haveMinMax || hi > maxD) maxD = hi;
                    haveMinMax = true;
                    break;
                }
                default:
                    // BYTE_ARRAY and others: no numeric min/max we store.
                    break;
            }
        }

        if (!haveNull && !haveMinMax) {
            continue;
        }

        auto& columnStats = stats.ColumnStats[descr->name()];
        if (!columnStats) {
            columnStats = std::make_shared<TStats::TColumnStats>();
        }
        if (haveNull) {
            columnStats->NullCount = static_cast<uint64_t>(nullCount);
        }
        if (haveMinMax) {
            if (isFloat) {
                columnStats->MinValue = std::bit_cast<uint64_t>(minD);
                columnStats->MaxValue = std::bit_cast<uint64_t>(maxD);
            } else {
                columnStats->MinValue = std::bit_cast<uint64_t>(minI);
                columnStats->MaxValue = std::bit_cast<uint64_t>(maxI);
            }
        }
    }
}

void TParquetFileData::LoadColumnStats()
{
    auto metadata = FileReader->parquet_reader()->metadata();

    // Row count is always available from the parquet footer, even for a plain
    // parquet with none of our custom metadata; per-column stats below are
    // best-effort on top of it.
    auto stats = std::make_shared<TStats>();
    stats->RowCount = static_cast<uint64_t>(metadata->num_rows());
    Stats = stats;

    // Base layer: min/max/null_count from the standard parquet column
    // statistics (present in most files). Our custom blob below adds NDV and
    // histograms on top.
    LoadStandardColumnStats(*stats);

    auto kv = metadata->key_value_metadata();
    if (!kv) {
        return;
    }
    const int keyIdx = kv->FindKey("stats");
    if (keyIdx < 0) {
        return;
    }

    auto parsed = llvm::json::parse(kv->value(keyIdx));
    if (!parsed) {
        llvm::consumeError(parsed.takeError());
        return;
    }
    const llvm::json::Object* root = parsed->getAsObject();
    const llvm::json::Array* columns = root ? root->getArray("columns") : nullptr;
    if (!columns) {
        return;
    }

    // Physical types decide how min/max/histogram bit patterns are stored.
    std::unordered_map<std::string_view, size_t> indexByName;
    indexByName.reserve(FileNames.size());
    for (size_t i = 0; i < FileNames.size(); ++i) {
        indexByName.emplace(FileNames[i], i);
    }

    for (const auto& colValue : *columns) {
        const llvm::json::Object* col = colValue.getAsObject();
        auto name = col ? col->getString("name") : std::nullopt;
        if (!name) {
            continue;
        }
        auto found = indexByName.find(std::string_view(name->data(), name->size()));
        if (found == indexByName.end()) {
            continue;
        }
        const size_t idx = found->second;

        const auto typeId =
            FileSchema->field(static_cast<int>(idx))->type()->id();
        const bool isFloat =
            typeId == arrow::Type::FLOAT || typeId == arrow::Type::DOUBLE;
        const bool isString =
            typeId == arrow::Type::STRING || typeId == arrow::Type::LARGE_STRING;

        // Store the raw 8-byte pattern matching the column's type: doubles keep
        // their bit representation, every other numeric is widened to int64.
        auto encode = [&](const llvm::json::Value& v) -> std::optional<uint64_t> {
            if (isFloat) {
                if (auto d = v.getAsNumber()) {
                    return std::bit_cast<uint64_t>(*d);
                }
                return std::nullopt;
            }
            if (auto i = v.getAsInteger()) {
                return std::bit_cast<uint64_t>(*i);
            }
            if (auto d = v.getAsNumber()) {
                return std::bit_cast<uint64_t>(static_cast<int64_t>(*d));
            }
            return std::nullopt;
        };

        // Merge onto the standard stats already loaded for this column: the
        // blob owns NDV/histogram; min/max/null_count only fill gaps the
        // standard parquet statistics left empty.
        auto& columnStats = stats->ColumnStats[FileNames[idx]];
        if (!columnStats) {
            columnStats = std::make_shared<TStats::TColumnStats>();
        }
        if (auto ndv = col->getInteger("ndv")) {
            columnStats->Ndv = static_cast<uint64_t>(*ndv);
        }
        columnStats->NdvIsExact = col->getBoolean("ndv_exact").value_or(false);
        if (!columnStats->NullCount) {
            if (auto nullCount = col->getInteger("null_count")) {
                columnStats->NullCount = static_cast<uint64_t>(*nullCount);
            }
        }

        if (!isString) {
            if (!columnStats->MinValue) {
                if (const auto* minValue = col->get("min")) {
                    columnStats->MinValue = encode(*minValue);
                }
            }
            if (!columnStats->MaxValue) {
                if (const auto* maxValue = col->get("max")) {
                    columnStats->MaxValue = encode(*maxValue);
                }
            }
            if (const llvm::json::Array* hist = col->getArray("histogram")) {
                columnStats->Histogram.clear();
                columnStats->Histogram.reserve(hist->size());
                for (const auto& bucket : *hist) {
                    if (auto encoded = encode(bucket)) {
                        columnStats->Histogram.push_back(*encoded);
                    }
                }
            }
        }
    }
}

TParquetSource::~TParquetSource() = default;

const TStatsPtr TParquetSource::Stats() const
{
    return File_->Stats;
}

void TParquetSource::RestrictColumns(const std::unordered_set<std::string>& names) {
    RestrictedColumns_ = names;
    ResetReader();
}

std::vector<NScheduler::TScanRowGroup> TParquetSource::ScanRowGroups() const
{
    return File_->RowGroups;
}

std::vector<NScheduler::TScanRowGroup> TParquetSource::PruneRowGroups(
    const NQumir::NAst::TExprPtr& predicate,
    std::string_view sourceAlias,
    std::ostream* diagnostics) const
{
    if (!predicate) {
        return File_->RowGroups;
    }
    auto evaluator = TRowGroupPredicateEvaluator::Compile(
        predicate, File_->FullSchema, sourceAlias, diagnostics);
    if (!evaluator || File_->RowGroupRanges.size() != File_->RowGroups.size()) {
        return File_->RowGroups;
    }

    std::vector<NScheduler::TScanRowGroup> kept;
    kept.reserve(File_->RowGroups.size());
    for (size_t rg = 0; rg < File_->RowGroups.size(); ++rg) {
        const bool mayBeTrue = evaluator->MayBeTrue(File_->RowGroupRanges[rg]);
        if (diagnostics) {
            *diagnostics << "row-group predicate: rg=" << rg
                         << " may_be_true=" << mayBeTrue;
            for (size_t column = 0;
                 column < File_->RowGroupRanges[rg].size(); ++column)
            {
                const auto& range = File_->RowGroupRanges[rg][column];
                *diagnostics << " col=" << File_->FileNames[column]
                             << " bounds=" << range.HasBounds
                             << " min_bits=" << range.MinValue
                             << " max_bits=" << range.MaxValue
                             << " null=" << range.MayBeNull
                             << " value=" << range.MayBeValue;
            }
            *diagnostics << '\n';
        }
        if (mayBeTrue) {
            kept.push_back(File_->RowGroups[rg]);
        }
    }
    return kept;
}

std::unique_ptr<TParquetSource> TParquetSource::MakeRowGroupsSource(
    const std::vector<size_t>& selectedRowGroups) const
{
    std::vector<int> rowGroups;
    rowGroups.reserve(selectedRowGroups.size());
    for (size_t rowGroup : selectedRowGroups) {
        rowGroups.push_back(static_cast<int>(rowGroup));
    }
    return std::unique_ptr<TParquetSource>(new TParquetSource(
        File_, std::optional<std::vector<int>>(std::move(rowGroups)),
        RestrictedColumns_));
}

std::shared_ptr<const IPhysicalRowReader> TParquetSource::CompileReader(
    std::span<const std::string> columnNames) const
{
    return std::make_shared<TParquetPhysicalRowReader>(
        File_->FileReader, columnNames, GetMemoryPool());
}

const TSchema& TParquetSource::Schema() const {
    return Schema_;
}

bool TParquetSource::Next(TRowSet& rowSet) {
    std::shared_ptr<arrow::RecordBatch> batch;
    if (!Reader_->ReadNext(&batch).ok() || !batch) {
        return false;
    }
    RecordBatchToRowSet(std::move(batch), rowSet);
    return true;
}

} // namespace NQdb
