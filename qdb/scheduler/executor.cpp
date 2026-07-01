#include <qdb/scheduler/executor.h>

#include <qdb/scheduler/single_threaded_scheduler.h>
#include <qdb/scheduler/threaded_scheduler.h>

#include <algorithm>
#include <utility>

namespace NQdb {
namespace NScheduler {
namespace {

void SetError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

} // namespace

TSchedulerExecutor::TSchedulerExecutor(
    TTaskGraph& graph,
    TSettings settings)
    : Graph_(graph)
    , Settings_(settings)
{}

bool TSchedulerExecutor::Run(std::string* error) {
    if (Settings_.Scheduler.Mode == EExecutionMode::Serial) {
        SetError(error, "serial execution mode uses the current physical executor");
        return false;
    }

    if (Settings_.Scheduler.Mode == EExecutionMode::SingleThreadedScheduler) {
        TSingleThreadedScheduler scheduler(Graph_);
        return scheduler.Run(error);
    }

    if (Settings_.Scheduler.Mode == EExecutionMode::ThreadedScheduler) {
        TThreadedScheduler scheduler(
            Graph_,
            std::max<size_t>(Settings_.Scheduler.WorkerCount, 1),
            Settings_.Scheduler.ReadyQueueCapacity);
        return scheduler.Run(error);
    }

    SetError(error, "unknown scheduler execution mode");
    return false;
}

} // namespace NScheduler
} // namespace NQdb
