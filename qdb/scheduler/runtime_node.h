#pragma once

#include <qdb/exec/executor.h>
#include <qdb/scheduler/executor.h>
#include <qdb/scheduler/partitioner.h>
#include <qdb/scheduler/runtime_adapter.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace NQdb {
namespace NScheduler {

class TBufferedSchedulerOutput {
public:
    ~TBufferedSchedulerOutput();

    void Add(const TRowSet& rowSet);
    bool Next(TRowSet& rowSet);

private:
    std::vector<TRowSet> RowSets_;
    size_t Next_ = 0;
};

std::shared_ptr<TSinkCode> MakeBufferedSchedulerSinkCode();

std::unique_ptr<IRuntimeNode> BuildBufferedSchedulerRuntimePipeline(
    TPipelinePartitionSpec spec,
    NQumir::NAst::TTypePtr outputType,
    std::string* error = nullptr);

std::unique_ptr<IRuntimeNode> BuildBufferedSchedulerJoinRuntimePipeline(
    TJoinPipelinePartitionSpec spec,
    NQumir::NAst::TTypePtr outputType,
    std::string* error = nullptr);

class TRuntimeSchedulerPipeline final : public IRuntimeNode {
public:
    TRuntimeSchedulerPipeline(
        std::unique_ptr<TTaskGraph> graph,
        TSettings settings,
        NQumir::NAst::TTypePtr outputType,
        std::shared_ptr<TBufferedSchedulerOutput> output);

    NQumir::NAst::TTypePtr OutputType() const override { return OutputType_; }
    bool Next(TRowSet& rowSet) override;

private:
    std::unique_ptr<TTaskGraph> Graph_;
    TSettings Settings_;
    NQumir::NAst::TTypePtr OutputType_;
    std::shared_ptr<TBufferedSchedulerOutput> Output_;
    bool Started_ = false;
};

} // namespace NScheduler
} // namespace NQdb
