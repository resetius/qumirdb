#include <qdb/scheduler/single_threaded_scheduler.h>

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

TSingleThreadedScheduler::TSingleThreadedScheduler(TTaskGraph& graph)
    : Graph_(graph)
{}

bool TSingleThreadedScheduler::Run(std::string* error) {
    if (HasRun_) {
        SetError(error, "single-threaded scheduler cannot run twice");
        return false;
    }
    HasRun_ = true;

    if (!Graph_.Validate(error)) {
        return false;
    }

    Ready_.clear();
    Scheduled_.clear();
    Schedule(Graph_.Root());

    while (!Ready_.empty()) {
        auto* node = Ready_.front();
        Ready_.pop_front();
        Scheduled_.erase(node);

        auto state = node->Task->Execute();
        if (state == ETaskResult::NEED_DATA) {
            ScheduleInput(node);
        } else if (
            state == ETaskResult::BLOCKED_OUTPUT ||
            state == ETaskResult::FINISHED)
        {
            ScheduleOutput(node);
        } else if (state == ETaskResult::OK) {
            Schedule(node);
            ScheduleOutput(node);
        }
    }

    return true;
}

void TSingleThreadedScheduler::Schedule(TTaskNode* node) {
    if (!node || Scheduled_.contains(node)) {
        return;
    }
    Ready_.push_back(node);
    Scheduled_.insert(node);
}

void TSingleThreadedScheduler::ScheduleInput(TTaskNode* node) {
    for (auto* edge : node->Inbound) {
        Schedule(edge->Src);
    }
}

void TSingleThreadedScheduler::ScheduleOutput(TTaskNode* node) {
    for (auto* edge : node->Outbound) {
        Schedule(edge->Dst);
    }
}

} // namespace NScheduler
} // namespace NQdb
