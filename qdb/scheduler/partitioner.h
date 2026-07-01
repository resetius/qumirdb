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

struct TSourcePartitionSpec {
    std::shared_ptr<const TSourceCode> Code;
    TPartitionStateFactory MakeState;
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

struct TPipelinePartitionSpec {
    TSourcePartitionSpec Source;
    std::vector<TUnaryPartitionSpec> UnaryStages;
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

} // namespace NScheduler
} // namespace NQdb
