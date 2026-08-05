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
    Stats_ = {};
    Schedule(Graph_.Root());

    while (!Ready_.empty()) {
        auto* node = Ready_.front();
        Ready_.pop_front();
        Scheduled_.erase(node);
        ++Stats_.Popped;

        auto state = node->Task->Execute();
        ++Stats_.Executed;
        if (state == ETaskResult::NEED_DATA) {
            ++Stats_.NeedData;
            ScheduleInput(node);
        } else if (
            state == ETaskResult::BLOCKED_OUTPUT ||
            state == ETaskResult::FINISHED)
        {
            if (state == ETaskResult::BLOCKED_OUTPUT) {
                ++Stats_.BlockedOutput;
            } else {
                ++Stats_.Finished;
            }
            ScheduleOutput(node);
        } else if (state == ETaskResult::OK) {
            ++Stats_.Ok;
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
    ++Stats_.Scheduled;
}

void TSingleThreadedScheduler::ScheduleInput(TTaskNode* node) {
    for (auto* edge : node->Inbound) {
        if (!node->Task->WantsInput(*edge)) {
            continue;
        }
        Schedule(edge->Src);
    }
}

void TSingleThreadedScheduler::ScheduleOutput(TTaskNode* node) {
    for (auto* edge : node->Outbound) {
        Schedule(edge->Dst);
    }
}

TSchedulerRunStats TSingleThreadedScheduler::Stats() const {
    return Stats_;
}

} // namespace NScheduler
} // namespace NQdb
