#pragma once

#include <qdb/io/io.h>
#include <qdb/plan/ops/stats.h>
#include <qdb/scheduler/scan_split.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace arrow { class RecordBatchReader; }

namespace NQdb {

class TParquetSource;
struct TParquetFileData;

// Opened parquet file shared by all of its scan sources. The file schema and
// statistics are loaded once; each source creates its own record-batch reader.
class TParquetFile {
public:
    explicit TParquetFile(const std::string& path);
    ~TParquetFile();

    std::unique_ptr<TParquetSource> MakeSource() const;

private:
    std::shared_ptr<TParquetFileData> Data_;
};

class TParquetSource : public ISource, public NScheduler::IScanMetadataSource {
public:
    explicit TParquetSource(const std::string& path);
    ~TParquetSource();

    const TSchema& Schema() const override;
    bool Next(TRowSet& rowSet) override;
    void RestrictColumns(const std::unordered_set<std::string>& names) override;
    std::vector<NScheduler::TScanRowGroup> ScanRowGroups() const override;
    std::unique_ptr<TParquetSource> MakeRowGroupRangeSource(
        size_t firstRowGroup,
        size_t rowGroupCount) const;

    const TStatsPtr Stats() const override;

private:
    friend class TParquetFile;
    TParquetSource(
        std::shared_ptr<TParquetFileData> file,
        std::vector<int> rowGroups,
        std::optional<std::unordered_set<std::string>> restrictedColumns);

    void ResetReader();
    std::vector<int> EffectiveRowGroups() const;
    std::vector<int> EffectiveColumnIndices() const;
    void RefreshSchema();

    std::shared_ptr<TParquetFileData> File_;
    std::vector<int> RowGroups_;
    std::optional<std::unordered_set<std::string>> RestrictedColumns_;
    std::shared_ptr<arrow::RecordBatchReader> Reader_;
    std::vector<std::string> Names_;
    std::vector<TColumnSchema> Columns_;
    TSchema Schema_;
};

} // namespace NQdb
