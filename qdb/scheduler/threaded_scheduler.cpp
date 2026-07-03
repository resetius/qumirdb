#include <qdb/scheduler/threaded_scheduler.h>

#include <algorithm>
#include <cassert>
#include <thread>
#include <utility>
#include <vector>

namespace NQdb {
namespace NScheduler {
namespace {

void SetError(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
}

} // namespace

TThreadedScheduler::TThreadedScheduler(
    TTaskGraph& graph,
    size_t workerCount,
    size_t readyQueueCapacity)
    : Graph_(graph)
    , Ready_(std::max({
        readyQueueCapacity,
        graph.Nodes().size(),
        size_t{1}}))
    , WorkerCount_(std::max<size_t>(workerCount, 1))
{}

bool TThreadedScheduler::Run(std::string* error) {
    if (HasRun_) {
        SetError(error, "threaded scheduler cannot run twice");
        return false;
    }
    HasRun_ = true;

    Graph_.Build();
    if (!Graph_.Validate(error)) {
        return false;
    }

    FinishedCount_.store(0, std::memory_order_release);
    ResetStats();
    Schedule(Graph_.Root());

    std::vector<std::thread> workers;
    workers.reserve(WorkerCount_);
    for (size_t i = 0; i < WorkerCount_; ++i) {
        workers.emplace_back([this]() {
            Work();
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    return true;
}

void TThreadedScheduler::Schedule(TTaskNode* node) {
    if (!node) {
        return;
    }

    auto state = node->State.load(std::memory_order_acquire);
    for (;;) {
        if (state == ETaskState::Idle) {
            if (node->State.compare_exchange_weak(
                    state,
                    ETaskState::Queued,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break;
            }
        } else if (state == ETaskState::Running) {
            if (node->State.compare_exchange_weak(
                    state,
                    ETaskState::Reschedule,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return;
            }
        } else {
            return;
        }
    }

    while (!Ready_.TryPush(std::move(node))) {
        ReadyPushRetries_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
    }
    Scheduled_.fetch_add(1, std::memory_order_relaxed);
    // Wake one idle worker (release synchronizes with the acquiring worker, so
    // the pushed node is visible before it pops).
    WorkAvailable_.release();
}

TTaskNode* TThreadedScheduler::PopReady() {
    TTaskNode* node = nullptr;
    if (Ready_.TryPop(node)) {
        Popped_.fetch_add(1, std::memory_order_relaxed);
    } else {
        EmptyReadyPolls_.fetch_add(1, std::memory_order_relaxed);
    }
    return node;
}

void TThreadedScheduler::ScheduleInput(TTaskNode* node) {
    bool allFinished = !node->Inbound.empty();
    for (auto* edge : node->Inbound) {
        if (edge->Src->State.load(std::memory_order_acquire) ==
            ETaskState::Finished)
        {
            continue;
        }
        allFinished = false;
        Schedule(edge->Src);
    }
    if (allFinished) {
        Schedule(node);
    }
}

void TThreadedScheduler::ScheduleOutput(TTaskNode* node) {
    for (auto* edge : node->Outbound) {
        Schedule(edge->Dst);
    }
}

void TThreadedScheduler::Work() {
    const size_t nodeCount = Graph_.Nodes().size();
    // Brief spin on the work permit before blocking, so bursts of scheduling
    // are picked up without an OS wakeup; then park on the semaphore.
    constexpr int acquireSpin = 64;
    for (;;) {
        if (FinishedCount_.load(std::memory_order_acquire) == nodeCount) {
            return;
        }
        bool gotPermit = false;
        for (int i = 0; i < acquireSpin; ++i) {
            if (WorkAvailable_.try_acquire()) {
                gotPermit = true;
                break;
            }
        }
        if (!gotPermit) {
            WorkAvailable_.acquire();
        }
        if (FinishedCount_.load(std::memory_order_acquire) == nodeCount) {
            return;
        }

        // A permit means a node was enqueued; release/acquire ordering makes it
        // visible, so this pops immediately (the loop only guards a transient
        // MPMC gap or the termination wakeup).
        auto* node = PopReady();
        while (!node) {
            if (FinishedCount_.load(std::memory_order_acquire) == nodeCount) {
                return;
            }
            node = PopReady();
        }

        auto expected = ETaskState::Queued;
        if (!node->State.compare_exchange_strong(
                expected,
                ETaskState::Running,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }

        auto result = node->Task->Execute();
        Executed_.fetch_add(1, std::memory_order_relaxed);
        if (result == ETaskResult::NEED_DATA) {
            NeedData_.fetch_add(1, std::memory_order_relaxed);
            auto reschedule = FinishRun(node);
            ScheduleInput(node);
            if (reschedule) {
                Rescheduled_.fetch_add(1, std::memory_order_relaxed);
                Schedule(node);
            }
        } else if (result == ETaskResult::BLOCKED_OUTPUT) {
            BlockedOutput_.fetch_add(1, std::memory_order_relaxed);
            auto reschedule = FinishRun(node);
            ScheduleOutput(node);
            if (reschedule) {
                Rescheduled_.fetch_add(1, std::memory_order_relaxed);
                Schedule(node);
            }
        } else if (result == ETaskResult::FINISHED) {
            Finished_.fetch_add(1, std::memory_order_relaxed);
            FinishNode(node);
            ScheduleOutput(node);
        } else if (result == ETaskResult::OK) {
            Ok_.fetch_add(1, std::memory_order_relaxed);
            FinishRun(node);
            Schedule(node);
            ScheduleOutput(node);
        }
    }
}

bool TThreadedScheduler::FinishRun(TTaskNode* node) {
    auto previous = node->State.exchange(ETaskState::Idle, std::memory_order_acq_rel);
    assert(previous == ETaskState::Running || previous == ETaskState::Reschedule);
    return previous == ETaskState::Reschedule;
}

void TThreadedScheduler::FinishNode(TTaskNode* node) {
    auto previous = node->State.exchange(ETaskState::Finished, std::memory_order_acq_rel);
    assert(previous == ETaskState::Running || previous == ETaskState::Reschedule);
    auto finished = FinishedCount_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (finished == Graph_.Nodes().size()) {
        // Graph is complete: wake every worker so they observe termination and
        // exit instead of blocking on the permit forever.
        WorkAvailable_.release(static_cast<ptrdiff_t>(WorkerCount_));
    }
}

void TThreadedScheduler::ResetStats() {
    Scheduled_.store(0, std::memory_order_relaxed);
    Popped_.store(0, std::memory_order_relaxed);
    Executed_.store(0, std::memory_order_relaxed);
    NeedData_.store(0, std::memory_order_relaxed);
    BlockedOutput_.store(0, std::memory_order_relaxed);
    Ok_.store(0, std::memory_order_relaxed);
    Finished_.store(0, std::memory_order_relaxed);
    Rescheduled_.store(0, std::memory_order_relaxed);
    ReadyPushRetries_.store(0, std::memory_order_relaxed);
    EmptyReadyPolls_.store(0, std::memory_order_relaxed);
}

TSchedulerRunStats TThreadedScheduler::Stats() const {
    return TSchedulerRunStats{
        .Scheduled = Scheduled_.load(std::memory_order_relaxed),
        .Popped = Popped_.load(std::memory_order_relaxed),
        .Executed = Executed_.load(std::memory_order_relaxed),
        .NeedData = NeedData_.load(std::memory_order_relaxed),
        .BlockedOutput = BlockedOutput_.load(std::memory_order_relaxed),
        .Ok = Ok_.load(std::memory_order_relaxed),
        .Finished = Finished_.load(std::memory_order_relaxed),
        .Rescheduled = Rescheduled_.load(std::memory_order_relaxed),
        .ReadyPushRetries = ReadyPushRetries_.load(std::memory_order_relaxed),
        .EmptyReadyPolls = EmptyReadyPolls_.load(std::memory_order_relaxed),
    };
}

} // namespace NScheduler
} // namespace NQdb
