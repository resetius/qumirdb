#include <qdb/scheduler/runtime_node.h>

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

std::unique_ptr<IRuntimeNode> BuildBufferedSchedulerRuntimePipeline(
    TPipelinePartitionSpec spec,
    NQumir::NAst::TTypePtr outputType,
    std::string* error)
{
    auto output = std::make_shared<TBufferedSchedulerOutput>();
    spec.Sink = TSinkPartitionSpec{
        .Code = MakeBufferedSchedulerSinkCode(),
        .MakeState = [output]() {
            return output;
        },
    };

    auto partitioned = TPipelinePartitioner::Build(spec, error);
    if (!partitioned.Graph) {
        return {};
    }

    return std::make_unique<TRuntimeSchedulerPipeline>(
        std::move(partitioned.Graph),
        spec.Settings,
        std::move(outputType),
        std::move(output));
}

TRuntimeSchedulerPipeline::TRuntimeSchedulerPipeline(
    std::unique_ptr<TTaskGraph> graph,
    TSettings settings,
    NQumir::NAst::TTypePtr outputType,
    std::shared_ptr<TBufferedSchedulerOutput> output)
    : Graph_(std::move(graph))
    , Settings_(settings)
    , OutputType_(std::move(outputType))
    , Output_(std::move(output))
{}

bool TRuntimeSchedulerPipeline::Next(TRowSet& rowSet)
{
    if (!Started_) {
        Started_ = true;
        std::string error;
        TSchedulerExecutor executor(*Graph_, Settings_);
        if (!executor.Run(&error)) {
            throw std::runtime_error(error);
        }
    }
    return Output_->Next(rowSet);
}

} // namespace NScheduler
} // namespace NQdb
