#include <qdb/scheduler/graph.h>

#include <format>
#include <unordered_set>
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

TTaskNode& TTaskGraph::AddNode(ITaskNode& task) {
    auto node = std::make_unique<TTaskNode>();
    node->Task = &task;
    auto* ptr = node.get();
    Nodes_.push_back(std::move(node));
    return *ptr;
}

TTaskNode& TTaskGraph::AddOwnedNode(std::unique_ptr<ITaskNode> task) {
    auto* ptr = task.get();
    OwnedTasks_.push_back(std::move(task));
    return AddNode(*ptr);
}

TTaskEdge& TTaskGraph::AddEdge(
    TTaskNode& src,
    TTaskNode& dst,
    std::unique_ptr<IConnection> connection,
    size_t srcLane,
    size_t dstLane)
{
    auto edge = std::make_unique<TTaskEdge>();
    edge->Src = &src;
    edge->Dst = &dst;
    edge->Connection = std::move(connection);
    edge->SrcLane = srcLane;
    edge->DstLane = dstLane;
    auto* ptr = edge.get();
    Edges_.push_back(std::move(edge));
    return *ptr;
}

void TTaskGraph::Build() {
    Leaves_.clear();
    Root_ = nullptr;

    for (auto& node : Nodes_) {
        node->Inbound.clear();
        node->Outbound.clear();
        node->State.store(ETaskState::Idle, std::memory_order_release);
    }

    for (auto& edge : Edges_) {
        edge->Src->Outbound.push_back(edge.get());
        edge->Dst->Inbound.push_back(edge.get());
    }

    for (auto& node : Nodes_) {
        if (node->Inbound.empty()) {
            Leaves_.push_back(node.get());
        }
        if (node->Outbound.empty()) {
            Root_ = node.get();
        }
    }
}

bool TTaskGraph::Validate(std::string* error) const {
    if (Nodes_.empty()) {
        SetError(error, "task graph is empty");
        return false;
    }

    std::unordered_set<const TTaskNode*> nodes;
    nodes.reserve(Nodes_.size());
    size_t rootCount = 0;
    size_t leafCount = 0;
    for (auto& node : Nodes_) {
        if (!node->Task) {
            SetError(error, "task graph contains a node without a task");
            return false;
        }
        nodes.insert(node.get());
        if (node->Inbound.empty()) {
            ++leafCount;
        }
        if (node->Outbound.empty()) {
            ++rootCount;
        }
    }

    if (leafCount == 0) {
        SetError(error, "task graph has no source leaf");
        return false;
    }
    if (rootCount != 1) {
        SetError(error, std::format(
            "task graph must have exactly one root, got {}", rootCount));
        return false;
    }

    for (auto& edge : Edges_) {
        if (!edge->Src || !edge->Dst) {
            SetError(error, "task graph contains an edge without endpoints");
            return false;
        }
        if (!nodes.contains(edge->Src) || !nodes.contains(edge->Dst)) {
            SetError(error, "task graph contains an edge with unknown endpoints");
            return false;
        }
        if (!edge->Connection) {
            SetError(error, "task graph contains an edge without a connection");
            return false;
        }
        if (edge->SrcLane >= edge->Src->Outbound.size()) {
            SetError(error, "task graph contains an invalid source lane id");
            return false;
        }
        if (edge->DstLane >= edge->Dst->Inbound.size()) {
            SetError(error, "task graph contains an invalid destination lane id");
            return false;
        }
    }

    return true;
}

const std::vector<std::unique_ptr<TTaskNode>>& TTaskGraph::Nodes() const {
    return Nodes_;
}

const std::vector<std::unique_ptr<TTaskEdge>>& TTaskGraph::Edges() const {
    return Edges_;
}

const std::vector<TTaskNode*>& TTaskGraph::Leaves() const {
    return Leaves_;
}

TTaskNode* TTaskGraph::Root() const {
    return Root_;
}

} // namespace NScheduler
} // namespace NQdb
