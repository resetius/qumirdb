#pragma once

#include <qdb/io/parquet/row_id.h>

#include <arrow/result.h>

#include <memory>
#include <span>
#include <vector>

namespace arrow {
class MemoryPool;
class RecordBatch;
} // namespace arrow

namespace parquet::arrow {
class FileReader;
}

namespace NQdb {

class TParquetRowReader {
public:
    explicit TParquetRowReader(
        std::shared_ptr<parquet::arrow::FileReader> reader,
        std::vector<int> columnIndexes,
        arrow::MemoryPool* memoryPool = nullptr);
    ~TParquetRowReader();

    TParquetRowReader(const TParquetRowReader&) = delete;
    TParquetRowReader& operator=(const TParquetRowReader&) = delete;

    arrow::Result<std::shared_ptr<arrow::RecordBatch>> ReadRows(
        std::span<const TPhysicalRowId> rowIds) const;

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};

} // namespace NQdb
