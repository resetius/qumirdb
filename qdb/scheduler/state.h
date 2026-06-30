#pragma once

namespace NQdb {
namespace NScheduler {

enum class ETaskResult {
    OK = 0,
    NEED_DATA = 1,
    BLOCKED_OUTPUT = 2,
    FINISHED = 3,
};

enum class ETaskState {
    Idle = 0,
    Queued = 1,
    Running = 2,
    Reschedule = 3,
    Finished = 4,
};

class ITaskNode {
public:
    virtual ~ITaskNode() = default;

    virtual ETaskResult Execute() = 0;
};

} // namespace NScheduler
} // namespace NQdb
