#pragma once

#include <qdb/exec/executor.h>
#include <qdb/exec/join_exec.h>
#include <qdb/exec/unary_block_exec.h>
#include <qdb/kernel/compiler.h>
#include <qdb/plan/ops/sort.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace NQdb {

struct TTopSortState;
struct TTopSortScratch;

struct TSortColumnRef {
    int32_t Index = 0;
    NQumir::NAst::TTypePtr Type;
};

struct TSortRadixKernel {
    bool Enabled = false;
    TKernelCompiler::TSortRadixCompositeDispatch Dispatch;
    TKernelCompiler::TSortRadixCompositeNullableDispatch NullableDispatch;
};

TRuntimeUnaryBlockingKernel::TProcess MakeSortProcess(
    NQumir::NAst::TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel radixKernel,
    int64_t batchRows = kJoinOutputBatchRows);

TRuntimeUnaryBlockingKernel::TProcess MakeTopSortProcess(
    NQumir::NAst::TTypePtr outputType,
    std::vector<TSortKey> keys,
    std::vector<TSortColumnRef> keyColumns,
    TSortRadixKernel radixKernel,
    int64_t limit,
    int64_t batchRows = kJoinOutputBatchRows);

class TRuntimeLimit : public IRuntimeNode {
public:
    TRuntimeLimit(std::unique_ptr<IRuntimeNode> input,
        NQumir::NAst::TTypePtr outputType,
        int64_t limit,
        int64_t offset,
        int64_t batchRows = kJoinOutputBatchRows);

    NQumir::NAst::TTypePtr OutputType() const override { return OutputType_; }
    bool Next(TRowSet& rowSet) override;

private:
    std::unique_ptr<IRuntimeNode> Input_;
    NQumir::NAst::TTypePtr OutputType_;
    int64_t Limit_ = 0;
    int64_t Offset_ = 0;
    int64_t BatchRows_ = kJoinOutputBatchRows;
    int64_t Skipped_ = 0;
    int64_t Emitted_ = 0;
};

} // namespace NQdb
