#pragma once

#include <qdb/io/io.h>
#include <qdb/scheduler/scan_split.h>

#include <memory>
#include <string>
#include <unordered_set>
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
    std::vector<NScheduler::TScanRowGroup> ScanRowGroups() const;
    std::unique_ptr<TParquetSource> MakeRowGroupRangeSource(
        size_t firstRowGroup,
        size_t rowGroupCount) const;

private:
    TParquetSource(
        const std::string& path,
        std::vector<int> rowGroups);

    void Open();
    void ResetReader();
    std::vector<int> EffectiveRowGroups() const;
    std::vector<int> EffectiveColumnIndices() const;
    void RefreshSchema();

    std::string Path_;
    std::vector<int> RowGroups_;
    std::unordered_set<std::string> RestrictedColumns_;
    std::unique_ptr<parquet::arrow::FileReader> FileReader_;
    std::shared_ptr<arrow::RecordBatchReader> Reader_;
    // Full-file column names in file order; stable across restrictions and used
    // to map RestrictedColumns_ to file column indices.
    std::vector<std::string> FileNames_;
    std::vector<std::string> Names_;
    std::vector<TColumnSchema> Columns_;
    TSchema Schema_;
};

} // namespace NQdb
