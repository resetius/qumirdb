#include <qdb/scheduler/runtime_node.h>

#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace NQdb {
namespace NScheduler {

TBufferedSchedulerOutput::~TBufferedSchedulerOutput()
{
    for (size_t i = Next_; i < RowSets_.size(); ++i) {
        if (RowSets_[i].RefCount) {
            Release(&RowSets_[i]);
        }
    }
}

void TBufferedSchedulerOutput::Add(const TRowSet& rowSet)
{
    // RefCount is stored in TRowSet itself, so retain the sink-owned instance
    // before TSinkTask releases it and keep a single-owner buffered copy.
    auto* retained = const_cast<TRowSet*>(&rowSet);
    Retain(retained);

    auto copy = *retained;
    copy.RefCount = 1;
    RowSets_.push_back(copy);
}

bool TBufferedSchedulerOutput::Next(TRowSet& rowSet)
{
    if (Next_ >= RowSets_.size()) {
        return false;
    }
    rowSet = RowSets_[Next_];
    RowSets_[Next_] = {};
    ++Next_;
    return true;
}

std::shared_ptr<TSinkCode> MakeBufferedSchedulerSinkCode()
{
    return std::make_shared<TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            auto* output = static_cast<TBufferedSchedulerOutput*>(state);
            output->Add(rowSet);
        });
}

TRuntimeSchedulerPipeline::TRuntimeSchedulerPipeline(
    std::unique_ptr<TTaskGraph> graph,
    TSettings settings,
    NQumir::NAst::TTypePtr outputType,
    std::shared_ptr<TBufferedSchedulerOutput> output,
    std::ostream* diagnostics)
    : Graph_(std::move(graph))
    , Settings_(settings)
    , OutputType_(std::move(outputType))
    , Output_(std::move(output))
    , Diagnostics_(diagnostics)
{}

bool TRuntimeSchedulerPipeline::Next(TRowSet& rowSet)
{
    if (!Started_) {
        Started_ = true;
        std::string error;
        Graph_->SetConnectionStatsEnabled(Settings_.Queue.EnableDebugCounters);
        TSchedulerExecutor executor(*Graph_, Settings_);
        if (!executor.Run(&error)) {
            throw std::runtime_error(error);
        }
        if (Settings_.Queue.EnableDebugCounters && Diagnostics_) {
            auto stats = executor.Stats();
            *Diagnostics_ << "\n========== SCHEDULER COUNTERS ==========\n";
            *Diagnostics_
                << "scheduled=" << stats.Scheduled
                << " popped=" << stats.Popped
                << " executed=" << stats.Executed
                << " ok=" << stats.Ok
                << " need_data=" << stats.NeedData
                << " blocked_output=" << stats.BlockedOutput
                << " finished=" << stats.Finished
                << " rescheduled=" << stats.Rescheduled
                << " ready_push_retries=" << stats.ReadyPushRetries
                << " empty_ready_polls=" << stats.EmptyReadyPolls
                << "\n";
            Graph_->PrintConnectionStats(*Diagnostics_);
            *Diagnostics_ << "========================================\n";
        }
    }
    return Output_->Next(rowSet);
}

} // namespace NScheduler
} // namespace NQdb
