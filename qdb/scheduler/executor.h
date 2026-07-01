#pragma once

#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/settings.h>

#include <string>

namespace NQdb {
namespace NScheduler {

class TSchedulerExecutor {
public:
    TSchedulerExecutor(
        TTaskGraph& graph,
        TSettings settings);

    bool Run(std::string* error = nullptr);

private:
    TTaskGraph& Graph_;
    TSettings Settings_;
};

} // namespace NScheduler
} // namespace NQdb
