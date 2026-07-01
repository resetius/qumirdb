#pragma once

#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/runtime_adapter.h>
#include <qdb/scheduler/scan_split.h>
#include <qdb/scheduler/settings.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace NQdb {
namespace NScheduler {

using TPartitionStateFactory = std::function<std::shared_ptr<void>(size_t)>;
using TSourcePartitionStateFactory = std::function<std::shared_ptr<void>(
    size_t,
    const TScanSplit*)>;

struct TSourcePartitionSpec {
    std::shared_ptr<const TSourceCode> Code;
    TSourcePartitionStateFactory MakeState;
    std::vector<TScanSplit> Splits;
};

struct TUnaryPartitionSpec {
    std::shared_ptr<const TUnaryCode> Code;
    TPartitionStateFactory MakeState;
};

struct TSinkPartitionSpec {
    std::shared_ptr<const TSinkCode> Code;
    std::function<std::shared_ptr<void>()> MakeState;
};

struct TBlockingPartitionSpec {
    std::shared_ptr<const TBlockingCode> Code;
    std::function<std::shared_ptr<void>()> MakeState;
};

struct TPipelinePartitionSpec {
    TSourcePartitionSpec Source;
    std::vector<TUnaryPartitionSpec> UnaryStages;
    TBlockingPartitionSpec BlockingTail;
    TSinkPartitionSpec Sink;
    TSettings Settings;
};

struct TPipelineSidePartitionSpec {
    TSourcePartitionSpec Source;
    std::vector<TUnaryPartitionSpec> UnaryStages;
};

struct THashShufflePartitionSpec {
    std::shared_ptr<const THashShuffleCode> Code;
    TPartitionStateFactory MakeState;
};

struct TBinaryPartitionSpec {
    std::shared_ptr<const TBinaryBlockingCode> Code;
    TPartitionStateFactory MakeState;
};

struct TJoinPipelinePartitionSpec {
    TPipelineSidePartitionSpec Left;
    TPipelineSidePartitionSpec Right;
    THashShufflePartitionSpec LeftShuffle;
    THashShufflePartitionSpec RightShuffle;
    TBinaryPartitionSpec Join;
    TSinkPartitionSpec Sink;
    TSettings Settings;
};

struct TPartitionedPipeline {
    std::unique_ptr<TTaskGraph> Graph;
    std::string Debug;
};

class TPipelinePartitioner {
public:
    static TPartitionedPipeline Build(
        const TPipelinePartitionSpec& spec,
        std::string* error = nullptr);
};

class TJoinPipelinePartitioner {
public:
    static TPartitionedPipeline Build(
        const TJoinPipelinePartitionSpec& spec,
        std::string* error = nullptr);
};

} // namespace NScheduler
} // namespace NQdb
