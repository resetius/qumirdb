#pragma once

#include <qdb/io/io.h>

#include <memory>
#include <string>
#include <vector>

namespace arrow { class RecordBatchReader; }
namespace parquet { namespace arrow { class FileReader; } }

namespace NQdb {

class TParquetSource : public ISource {
public:
    explicit TParquetSource(const std::string& path);
    ~TParquetSource();

    const TSchema& Schema() const override;
    bool Next(TRowSet& rowSet) override;
    void RestrictColumns(const std::unordered_set<std::string>& names) override;

private:
    std::unique_ptr<parquet::arrow::FileReader> FileReader_;
    std::shared_ptr<arrow::RecordBatchReader> Reader_;
    std::vector<std::string> Names_;
    std::vector<TColumnSchema> Columns_;
    TSchema Schema_;
};

} // namespace NQdb
