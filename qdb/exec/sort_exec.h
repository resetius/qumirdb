#pragma once

#include <qdb/exec/executor.h>
#include <qdb/exec/join_exec.h>
#include <qdb/kernel/compiler.h>
#include <qdb/plan/ops/sort.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace NQdb {

struct TSortColumnRef {
    int32_t Index = 0;
    NQumir::NAst::TTypePtr Type;
};

struct TSortRadixKey {
    bool Enabled = false;
    TKernelCompiler::TSortRadixDispatch Dispatch;
};

class TRuntimeSort : public IRuntimeNode {
public:
    TRuntimeSort(std::unique_ptr<IRuntimeNode> input,
        NQumir::NAst::TTypePtr outputType,
        std::vector<TSortKey> keys,
        std::vector<TSortColumnRef> keyColumns,
        std::vector<TSortRadixKey> radixKeys,
        int64_t batchRows = kJoinOutputBatchRows);

    NQumir::NAst::TTypePtr OutputType() const override { return OutputType_; }
    bool Next(TRowSet& rowSet) override;

private:
    void Materialize();
    bool TryRadixSort();

    std::unique_ptr<IRuntimeNode> Input_;
    NQumir::NAst::TTypePtr OutputType_;
    std::vector<TSortKey> Keys_;
    std::vector<TSortColumnRef> KeyColumns_;
    std::vector<TSortRadixKey> RadixKeys_;
    int64_t BatchRows_ = kJoinOutputBatchRows;

    bool Materialized_ = false;
    TRowStore Store_;
    std::vector<TRowId> Rows_;
    size_t Cursor_ = 0;
};

} // namespace NQdb
