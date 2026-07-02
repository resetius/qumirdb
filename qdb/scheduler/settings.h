#pragma once

#include <cstddef>

namespace NQdb {
namespace NScheduler {

enum class EExecutionMode {
    Serial = 0,
    SingleThreadedScheduler = 1,
    ThreadedScheduler = 2,
};

enum class EScanSplitStrategy {
    Auto = 0,
    RowGroupRange = 1,
    SerialRead = 2,
};

struct TSchedulerSettings {
    EExecutionMode Mode = EExecutionMode::Serial;
    size_t WorkerCount = 1;
    size_t ReadyQueueCapacity = 1024;
};

struct TPartitioningSettings {
    size_t DefaultPartitionCount = 1;
    size_t MaxPartitionCount = 1;
    size_t MinSplitRows = 0;
    size_t MinSplitBytes = 0;
};

struct TQueueSettings {
    size_t RowsetCapacityPerLane = 1;
    size_t MaxQueuedBytes = 0;
    bool EnableDebugCounters = false;
};

struct TScanSplitSettings {
    EScanSplitStrategy Strategy = EScanSplitStrategy::Auto;
    size_t TargetRowsPerTask = 0;
    size_t TargetBytesPerTask = 0;
    size_t MinRowsPerTask = 0;
    size_t MaxScanTasks = 1;
    size_t RowGroupCoalescingFactor = 1;
    size_t TinyInputRowsThreshold = 0;
};

struct THashShuffleSettings {
    size_t PartitionCount = 0;
    size_t MaxPartitionCount = 0;
    size_t MaxProducerLanes = 1;
    size_t MaxQueuedRowsetsPerLane = 1;
    size_t MaxQueuedBytesPerLane = 0;
    size_t TargetOutputBatchRows = 16 * 1024;
    size_t MaxOutputBatchRows = 64 * 1024;
    size_t TargetOutputBatchBytes = 1024 * 1024;
    size_t SerialFallbackRowsThreshold = 0;
};

struct TSortSettings {
    size_t LocalPartitionCount = 1;
    size_t MergeFanIn = 2;
    size_t MergeBatchRows = 0;
    size_t LocalTopLimitMultiplier = 1;
};

struct TKernelHelperSettings {
    bool EnableRowsetHashKernel = true;
    bool EnableCompareKernel = true;
    size_t ScratchVectorSize = 0;
};

struct TAggregateSettings {
    // Parallelize ungrouped/global aggregates with a partial->gather->combine
    // cascade instead of gathering every lane into one aggregate. Off by
    // default: the single-aggregate path is faster for cheap aggregates; the
    // cascade only pays off when the serial aggregate is the bottleneck (very
    // large inputs).
    bool CascadeGlobal = false;
};

struct TSettings {
    TSchedulerSettings Scheduler;
    TPartitioningSettings Partitioning;
    TQueueSettings Queue;
    TScanSplitSettings ScanSplit;
    THashShuffleSettings HashShuffle;
    TSortSettings Sort;
    TKernelHelperSettings KernelHelper;
    TAggregateSettings Aggregate;
};

} // namespace NScheduler
} // namespace NQdb
