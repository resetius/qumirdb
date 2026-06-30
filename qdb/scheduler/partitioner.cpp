#include <qdb/scheduler/partitioner.h>

#include <algorithm>
#include <format>
#include <sstream>
#include <utility>

namespace NQdb {
namespace NScheduler {
namespace {

void SetError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

size_t EffectivePartitionCount(const TSettings& settings) {
    auto partitions = std::max<size_t>(settings.Partitioning.DefaultPartitionCount, 1);
    auto maxPartitions = std::max<size_t>(settings.Partitioning.MaxPartitionCount, 1);
    return std::min(partitions, maxPartitions);
}

size_t EffectiveQueueCapacity(const TSettings& settings) {
    return std::max<size_t>(settings.Queue.RowsetCapacityPerLane, 1);
}

bool ValidateSpec(const TPipelinePartitionSpec& spec, std::string* error) {
    if (!spec.Source.Code) {
        SetError(error, "pipeline partition spec has no source code");
        return false;
    }
    if (!spec.Source.MakeState) {
        SetError(error, "pipeline partition spec has no source state factory");
        return false;
    }
    for (size_t i = 0; i < spec.UnaryStages.size(); ++i) {
        if (!spec.UnaryStages[i].Code) {
            SetError(error, std::format(
                "pipeline partition spec has no unary code at stage {}", i));
            return false;
        }
        if (!spec.UnaryStages[i].MakeState) {
            SetError(error, std::format(
                "pipeline partition spec has no unary state factory at stage {}", i));
            return false;
        }
    }
    if (!spec.Sink.Code) {
        SetError(error, "pipeline partition spec has no sink code");
        return false;
    }
    if (!spec.Sink.MakeState) {
        SetError(error, "pipeline partition spec has no sink state factory");
        return false;
    }
    return true;
}

void AppendCodeLine(
    std::ostringstream& out,
    const char* name,
    size_t partitions,
    const void* code)
{
    out << name
        << " partitions=" << partitions
        << " code=" << code
        << "\n";
}

} // namespace

TPartitionedPipeline TPipelinePartitioner::Build(
    const TPipelinePartitionSpec& spec,
    std::string* error)
{
    if (!ValidateSpec(spec, error)) {
        return {};
    }

    const auto partitionCount = EffectivePartitionCount(spec.Settings);
    const auto queueCapacity = EffectiveQueueCapacity(spec.Settings);

    auto graph = std::make_unique<TTaskGraph>();
    std::ostringstream debug;
    debug << "partition_count=" << partitionCount << "\n";
    debug << "queue_capacity=" << queueCapacity << "\n";
    AppendCodeLine(debug, "source", partitionCount, spec.Source.Code.get());
    for (size_t i = 0; i < spec.UnaryStages.size(); ++i) {
        auto name = std::format("unary[{}]", i);
        AppendCodeLine(
            debug,
            name.c_str(),
            partitionCount,
            spec.UnaryStages[i].Code.get());
    }
    AppendCodeLine(debug, "sink", 1, spec.Sink.Code.get());

    const size_t boundaryCount = spec.UnaryStages.size() + 1;
    std::vector<IConnection*> boundaries;
    boundaries.reserve(boundaryCount);
    for (size_t i = 0; i + 1 < boundaryCount; ++i) {
        auto conn = std::make_unique<TOneToOneConnection>(queueCapacity);
        conn->Resize(partitionCount, partitionCount);
        boundaries.push_back(&graph->AddConnection(std::move(conn)));
    }

    auto gather = std::make_unique<TGatherConnection>(queueCapacity);
    gather->Resize(partitionCount, 1);
    boundaries.push_back(&graph->AddConnection(std::move(gather)));

    std::vector<TTaskNode*> previous;
    previous.reserve(partitionCount);
    for (size_t partition = 0; partition < partitionCount; ++partition) {
        auto state = spec.Source.MakeState(partition);
        auto task = std::make_unique<TSourceTask>(
            spec.Source.Code,
            std::move(state),
            TOutputPort{
                .Connection = boundaries[0],
                .Lane = partition,
            });
        previous.push_back(&graph->AddOwnedNode(std::move(task)));
    }

    for (size_t stage = 0; stage < spec.UnaryStages.size(); ++stage) {
        std::vector<TTaskNode*> current;
        current.reserve(partitionCount);
        for (size_t partition = 0; partition < partitionCount; ++partition) {
            auto state = spec.UnaryStages[stage].MakeState(partition);
            auto task = std::make_unique<TUnaryTask>(
                spec.UnaryStages[stage].Code,
                std::move(state),
                TInputPort{
                    .Connection = boundaries[stage],
                    .Lane = partition,
                },
                TOutputPort{
                    .Connection = boundaries[stage + 1],
                    .Lane = partition,
                });
            current.push_back(&graph->AddOwnedNode(std::move(task)));
            graph->AddEdge(
                *previous[partition],
                *current.back(),
                *boundaries[stage],
                partition,
                partition);
            debug << "edge OneToOne"
                << " stage=" << stage
                << " partition=" << partition
                << " src_lane=" << partition
                << " dst_lane=" << partition
                << "\n";
        }
        previous = std::move(current);
    }

    auto sink = std::make_unique<TSinkTask>(
        spec.Sink.Code,
        spec.Sink.MakeState(),
        TInputPort{
            .Connection = boundaries.back(),
            .Lane = 0,
        });
    auto& sinkNode = graph->AddOwnedNode(std::move(sink));
    for (size_t partition = 0; partition < partitionCount; ++partition) {
        graph->AddEdge(
            *previous[partition],
            sinkNode,
            *boundaries.back(),
            partition,
            0);
        debug << "edge Gather"
            << " partition=" << partition
            << " src_lane=" << partition
            << " dst_lane=0"
            << "\n";
    }

    graph->Build();
    if (!graph->Validate(error)) {
        return {};
    }

    return TPartitionedPipeline{
        .Graph = std::move(graph),
        .Debug = debug.str(),
    };
}

} // namespace NScheduler
} // namespace NQdb
