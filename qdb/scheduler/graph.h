#pragma once

#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/state.h>

#include <atomic>
#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace NQdb {
namespace NScheduler {

struct TTaskEdge;

struct TTaskNode {
    ITaskNode* Task = nullptr;
    std::vector<TTaskEdge*> Inbound;
    std::vector<TTaskEdge*> Outbound;
    std::atomic<ETaskState> State = ETaskState::Idle;
};

struct TTaskEdge {
    TTaskNode* Src = nullptr;
    TTaskNode* Dst = nullptr;
    IConnection* Connection = nullptr;
    size_t SrcLane = 0;
    size_t DstLane = 0;
};

class TTaskGraph {
public:
    TTaskNode& AddNode(ITaskNode& task);
    TTaskNode& AddOwnedNode(std::unique_ptr<ITaskNode> task);
    IConnection& AddConnection(std::unique_ptr<IConnection> connection);

    TTaskEdge& AddEdge(
        TTaskNode& src,
        TTaskNode& dst,
        IConnection& connection,
        size_t srcLane = 0,
        size_t dstLane = 0);

    TTaskEdge& AddOwnedEdge(
        TTaskNode& src,
        TTaskNode& dst,
        std::unique_ptr<IConnection> connection,
        size_t srcLane = 0,
        size_t dstLane = 0);

    void Build();
    bool Validate(std::string* error = nullptr) const;
    void Print(std::ostream& out) const;

    const std::vector<std::unique_ptr<TTaskNode>>& Nodes() const;
    const std::vector<std::unique_ptr<TTaskEdge>>& Edges() const;
    const std::vector<TTaskNode*>& Leaves() const;
    TTaskNode* Root() const;

private:
    std::vector<std::unique_ptr<ITaskNode>> OwnedTasks_;
    std::vector<std::unique_ptr<IConnection>> OwnedConnections_;
    std::vector<std::unique_ptr<TTaskNode>> Nodes_;
    std::vector<std::unique_ptr<TTaskEdge>> Edges_;
    std::vector<TTaskNode*> Leaves_;
    TTaskNode* Root_ = nullptr;
};

} // namespace NScheduler
} // namespace NQdb
