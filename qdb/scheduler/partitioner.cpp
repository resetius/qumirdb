#include <qdb/scheduler/partitioner.h>

#include <algorithm>
#include <format>
#include <sstream>
#include <string_view>
#include <utility>

namespace NQdb {
namespace NScheduler {
namespace {

void SetError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

size_t EffectivePartitionCount(const TPipelinePartitionSpec& spec)
{
    if (!spec.Source.Splits.empty()) {
        return spec.Source.Splits.size();
    }

    auto partitions = std::max<size_t>(
        spec.Settings.Partitioning.DefaultPartitionCount,
        1);
    auto maxPartitions = std::max<size_t>(
        spec.Settings.Partitioning.MaxPartitionCount,
        1);
    return std::min(partitions, maxPartitions);
}

size_t EffectiveQueueCapacity(const TSettings& settings)
{
    return std::max<size_t>(settings.Queue.RowsetCapacityPerLane, 1);
}

bool ValidateSpec(const TPipelinePartitionSpec& spec, std::string* error)
{
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
    if (spec.BlockingTail.Code && !spec.BlockingTail.MakeState) {
        SetError(error, "pipeline partition spec has no blocking state factory");
        return false;
    }
    return true;
}

bool ValidateSideSpec(
    const TPipelineSidePartitionSpec& spec,
    std::string_view name,
    std::string* error)
{
    if (!spec.Source.Code) {
        SetError(error, std::format("{} pipeline side has no source code", name));
        return false;
    }
    if (!spec.Source.MakeState) {
        SetError(error, std::format("{} pipeline side has no source state factory", name));
        return false;
    }
    for (size_t i = 0; i < spec.UnaryStages.size(); ++i) {
        if (!spec.UnaryStages[i].Code) {
            SetError(error, std::format(
                "{} pipeline side has no unary code at stage {}", name, i));
            return false;
        }
        if (!spec.UnaryStages[i].MakeState) {
            SetError(error, std::format(
                "{} pipeline side has no unary state factory at stage {}", name, i));
            return false;
        }
    }
    return true;
}

bool ValidateJoinSpec(const TJoinPipelinePartitionSpec& spec, std::string* error)
{
    if (!ValidateSideSpec(spec.Left, "left", error)) {
        return false;
    }
    if (!ValidateSideSpec(spec.Right, "right", error)) {
        return false;
    }
    if (!spec.LeftShuffle.Code || !spec.LeftShuffle.MakeState) {
        SetError(error, "join pipeline has no left hash shuffle code or state factory");
        return false;
    }
    if (!spec.RightShuffle.Code || !spec.RightShuffle.MakeState) {
        SetError(error, "join pipeline has no right hash shuffle code or state factory");
        return false;
    }
    if (!spec.Join.Code || !spec.Join.MakeState) {
        SetError(error, "join pipeline has no binary join code or state factory");
        return false;
    }
    if (!spec.Sink.Code || !spec.Sink.MakeState) {
        SetError(error, "join pipeline has no sink code or state factory");
        return false;
    }
    return true;
}

size_t EffectiveSidePartitionCount(
    const TPipelineSidePartitionSpec& side,
    const TSettings& settings)
{
    if (!side.Source.Splits.empty()) {
        return side.Source.Splits.size();
    }

    auto partitions = std::max<size_t>(
        settings.Partitioning.DefaultPartitionCount,
        1);
    auto maxPartitions = std::max<size_t>(
        settings.Partitioning.MaxPartitionCount,
        1);
    return std::min(partitions, maxPartitions);
}

size_t EffectiveHashPartitionCount(const TSettings& settings)
{
    auto partitions = std::max<size_t>(settings.HashShuffle.PartitionCount, 1);
    auto maxPartitions = std::max<size_t>(settings.HashShuffle.MaxPartitionCount, 1);
    return std::min(partitions, maxPartitions);
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

struct TSideBuildResult {
    THashShuffleConnection* Shuffle = nullptr;
    std::vector<TTaskNode*> ShuffleNodes;
};

TSideBuildResult BuildJoinSide(
    TTaskGraph& graph,
    std::ostringstream& debug,
    std::string_view name,
    const TPipelineSidePartitionSpec& side,
    const THashShufflePartitionSpec& shuffle,
    size_t sidePartitions,
    size_t joinPartitions,
    size_t queueCapacity)
{
    auto hashShuffle = std::make_unique<THashShuffleConnection>(queueCapacity);
    auto* hashShufflePtr = hashShuffle.get();
    hashShuffle->Resize(sidePartitions, joinPartitions);
    graph.AddConnection(std::move(hashShuffle));

    debug << name << "_partition_count=" << sidePartitions << "\n";
    AppendCodeLine(
        debug,
        std::format("{}_source", name).c_str(),
        sidePartitions,
        side.Source.Code.get());
    for (size_t i = 0; i < side.UnaryStages.size(); ++i) {
        AppendCodeLine(
            debug,
            std::format("{}_unary[{}]", name, i).c_str(),
            sidePartitions,
            side.UnaryStages[i].Code.get());
    }
    AppendCodeLine(
        debug,
        std::format("{}_shuffle", name).c_str(),
        sidePartitions,
        shuffle.Code.get());

    std::vector<IConnection*> boundaries;
    boundaries.reserve(side.UnaryStages.size() + 1);
    for (size_t i = 0; i < side.UnaryStages.size() + 1; ++i) {
        auto conn = std::make_unique<TOneToOneConnection>(queueCapacity);
        conn->Resize(sidePartitions, sidePartitions);
        boundaries.push_back(&graph.AddConnection(std::move(conn)));
    }

    std::vector<TTaskNode*> previous;
    previous.reserve(sidePartitions);
    for (size_t partition = 0; partition < sidePartitions; ++partition) {
        const auto* split = side.Source.Splits.empty()
            ? nullptr
            : &side.Source.Splits[partition];
        auto task = std::make_unique<TSourceTask>(
            side.Source.Code,
            side.Source.MakeState(partition, split),
            TOutputPort{
                .Connection = boundaries[0],
                .Lane = partition,
            });
        previous.push_back(&graph.AddOwnedNode(std::move(task)));
    }

    for (size_t stage = 0; stage < side.UnaryStages.size(); ++stage) {
        std::vector<TTaskNode*> current;
        current.reserve(sidePartitions);
        for (size_t partition = 0; partition < sidePartitions; ++partition) {
            auto task = std::make_unique<TUnaryTask>(
                side.UnaryStages[stage].Code,
                side.UnaryStages[stage].MakeState(partition),
                TInputPort{
                    .Connection = boundaries[stage],
                    .Lane = partition,
                },
                TOutputPort{
                    .Connection = boundaries[stage + 1],
                    .Lane = partition,
                });
            current.push_back(&graph.AddOwnedNode(std::move(task)));
            graph.AddEdge(
                *previous[partition],
                *current.back(),
                *boundaries[stage],
                partition,
                partition);
            debug << "edge OneToOne"
                << " side=" << name
                << " stage=" << stage
                << " partition=" << partition
                << " src_lane=" << partition
                << " dst_lane=" << partition
                << "\n";
        }
        previous = std::move(current);
    }

    TSideBuildResult result;
    result.Shuffle = hashShufflePtr;
    result.ShuffleNodes.reserve(sidePartitions);
    for (size_t partition = 0; partition < sidePartitions; ++partition) {
        auto task = std::make_unique<THashShuffleTask>(
            shuffle.Code,
            shuffle.MakeState(partition),
            TInputPort{
                .Connection = boundaries.back(),
                .Lane = partition,
            },
            *hashShufflePtr,
            partition);
        result.ShuffleNodes.push_back(&graph.AddOwnedNode(std::move(task)));
        graph.AddEdge(
            *previous[partition],
            *result.ShuffleNodes.back(),
            *boundaries.back(),
            partition,
            partition);
        debug << "edge OneToOne"
            << " side=" << name
            << " stage=shuffle-input"
            << " partition=" << partition
            << " src_lane=" << partition
            << " dst_lane=" << partition
            << "\n";
    }

    return result;
}

} // namespace

TPartitionedPipeline TPipelinePartitioner::Build(
    const TPipelinePartitionSpec& spec,
    std::string* error)
{
    if (!ValidateSpec(spec, error)) {
        return {};
    }

    const auto partitionCount = EffectivePartitionCount(spec);
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
    if (spec.BlockingTail.Code) {
        AppendCodeLine(debug, "blocking", 1, spec.BlockingTail.Code.get());
    }

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
        const auto* split = spec.Source.Splits.empty()
            ? nullptr
            : &spec.Source.Splits[partition];
        auto state = spec.Source.MakeState(partition, split);
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

    if (spec.BlockingTail.Code) {
        auto blockingToSink = std::make_unique<TOneToOneConnection>(queueCapacity);
        blockingToSink->Resize(1, 1);
        auto* blockingToSinkPtr = &graph->AddConnection(std::move(blockingToSink));

        auto blocking = std::make_unique<TBlockingTask>(
            spec.BlockingTail.Code,
            spec.BlockingTail.MakeState(),
            TInputPort{
                .Connection = boundaries.back(),
                .Lane = 0,
            },
            TOutputPort{
                .Connection = blockingToSinkPtr,
                .Lane = 0,
            });
        auto& blockingNode = graph->AddOwnedNode(std::move(blocking));
        for (size_t partition = 0; partition < partitionCount; ++partition) {
            graph->AddEdge(
                *previous[partition],
                blockingNode,
                *boundaries.back(),
                partition,
                0);
            debug << "edge Gather"
                << " partition=" << partition
                << " src_lane=" << partition
                << " dst_lane=0"
                << "\n";
        }

        auto sink = std::make_unique<TSinkTask>(
            spec.Sink.Code,
            spec.Sink.MakeState(),
            TInputPort{
                .Connection = blockingToSinkPtr,
                .Lane = 0,
            });
        auto& sinkNode = graph->AddOwnedNode(std::move(sink));
        graph->AddEdge(
            blockingNode,
            sinkNode,
            *blockingToSinkPtr,
            0,
            0);
        debug << "edge OneToOne"
            << " stage=blocking"
            << " partition=0"
            << " src_lane=0"
            << " dst_lane=0"
            << "\n";

        graph->Build();
        if (!graph->Validate(error)) {
            return {};
        }

        return TPartitionedPipeline{
            .Graph = std::move(graph),
            .Debug = debug.str(),
        };
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

TPartitionedPipeline TJoinPipelinePartitioner::Build(
    const TJoinPipelinePartitionSpec& spec,
    std::string* error)
{
    if (!ValidateJoinSpec(spec, error)) {
        return {};
    }

    const auto queueCapacity = EffectiveQueueCapacity(spec.Settings);
    const auto leftPartitions = EffectiveSidePartitionCount(spec.Left, spec.Settings);
    const auto rightPartitions = EffectiveSidePartitionCount(spec.Right, spec.Settings);
    const auto joinPartitions = EffectiveHashPartitionCount(spec.Settings);

    auto graph = std::make_unique<TTaskGraph>();
    std::ostringstream debug;
    debug << "left_partition_count=" << leftPartitions << "\n";
    debug << "right_partition_count=" << rightPartitions << "\n";
    debug << "join_partition_count=" << joinPartitions << "\n";
    debug << "queue_capacity=" << queueCapacity << "\n";
    AppendCodeLine(debug, "join", joinPartitions, spec.Join.Code.get());
    AppendCodeLine(debug, "sink", 1, spec.Sink.Code.get());

    auto left = BuildJoinSide(
        *graph,
        debug,
        "left",
        spec.Left,
        spec.LeftShuffle,
        leftPartitions,
        joinPartitions,
        queueCapacity);
    auto right = BuildJoinSide(
        *graph,
        debug,
        "right",
        spec.Right,
        spec.RightShuffle,
        rightPartitions,
        joinPartitions,
        queueCapacity);

    auto gather = std::make_unique<TGatherConnection>(queueCapacity);
    gather->Resize(joinPartitions, 1);
    auto* gatherPtr = &graph->AddConnection(std::move(gather));

    std::vector<TTaskNode*> joinNodes;
    joinNodes.reserve(joinPartitions);
    for (size_t partition = 0; partition < joinPartitions; ++partition) {
        auto task = std::make_unique<TBinaryBlockingTask>(
            spec.Join.Code,
            spec.Join.MakeState(partition),
            TInputPort{
                .Connection = left.Shuffle,
                .Lane = partition,
            },
            TInputPort{
                .Connection = right.Shuffle,
                .Lane = partition,
            },
            TOutputPort{
                .Connection = gatherPtr,
                .Lane = partition,
            });
        joinNodes.push_back(&graph->AddOwnedNode(std::move(task)));

        for (size_t src = 0; src < leftPartitions; ++src) {
            graph->AddEdge(
                *left.ShuffleNodes[src],
                *joinNodes.back(),
                *left.Shuffle,
                src,
                partition);
            debug << "edge HashShuffle"
                << " side=left"
                << " src_lane=" << src
                << " dst_lane=" << partition
                << "\n";
        }
        for (size_t src = 0; src < rightPartitions; ++src) {
            graph->AddEdge(
                *right.ShuffleNodes[src],
                *joinNodes.back(),
                *right.Shuffle,
                src,
                partition);
            debug << "edge HashShuffle"
                << " side=right"
                << " src_lane=" << src
                << " dst_lane=" << partition
                << "\n";
        }
    }

    auto sink = std::make_unique<TSinkTask>(
        spec.Sink.Code,
        spec.Sink.MakeState(),
        TInputPort{
            .Connection = gatherPtr,
            .Lane = 0,
        });
    auto& sinkNode = graph->AddOwnedNode(std::move(sink));
    for (size_t partition = 0; partition < joinPartitions; ++partition) {
        graph->AddEdge(
            *joinNodes[partition],
            sinkNode,
            *gatherPtr,
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
