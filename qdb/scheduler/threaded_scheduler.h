#pragma once

#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/mpmc.h>

#include <atomic>
#include <cstddef>
#include <string>

namespace NQdb {
namespace NScheduler {

class TThreadedScheduler {
public:
    TThreadedScheduler(
        TTaskGraph& graph,
        size_t workerCount,
        size_t readyQueueCapacity = 0);

    bool Run(std::string* error = nullptr);

private:
    void Schedule(TTaskNode* node);
    TTaskNode* PopReady();
    void ScheduleInput(TTaskNode* node);
    void ScheduleOutput(TTaskNode* node);
    void Work();
    bool FinishRun(TTaskNode* node);
    void FinishNode(TTaskNode* node);

    TTaskGraph& Graph_;
    TMPMCQueue<TTaskNode*> Ready_;
    std::atomic<size_t> FinishedCount_ = 0;
    size_t WorkerCount_;
    bool HasRun_ = false;
};

} // namespace NScheduler
} // namespace NQdb
