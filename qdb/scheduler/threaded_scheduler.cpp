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
    , Ready_(readyQueueCapacity
        ? readyQueueCapacity
        : std::max<size_t>(graph.Nodes().size(), 1))
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

    auto pushed = Ready_.TryPush(std::move(node));
    assert(pushed);
}

TTaskNode* TThreadedScheduler::PopReady() {
    TTaskNode* node = nullptr;
    Ready_.TryPop(node);
    return node;
}

void TThreadedScheduler::ScheduleInput(TTaskNode* node) {
    for (auto* edge : node->Inbound) {
        Schedule(edge->Src);
    }
}

void TThreadedScheduler::ScheduleOutput(TTaskNode* node) {
    for (auto* edge : node->Outbound) {
        Schedule(edge->Dst);
    }
}

void TThreadedScheduler::Work() {
    auto nodeCount = Graph_.Nodes().size();
    while (FinishedCount_.load(std::memory_order_acquire) != nodeCount) {
        auto* node = PopReady();
        if (!node) {
            std::this_thread::yield();
            continue;
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
        if (result == ETaskResult::NEED_DATA) {
            auto reschedule = FinishRun(node);
            ScheduleInput(node);
            if (reschedule) {
                Schedule(node);
            }
        } else if (result == ETaskResult::BLOCKED_OUTPUT) {
            auto reschedule = FinishRun(node);
            ScheduleOutput(node);
            if (reschedule) {
                Schedule(node);
            }
        } else if (result == ETaskResult::FINISHED) {
            FinishNode(node);
            ScheduleOutput(node);
        } else if (result == ETaskResult::OK) {
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
    FinishedCount_.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace NScheduler
} // namespace NQdb
