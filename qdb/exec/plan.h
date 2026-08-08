#pragma once

#include <qdb/exec/stage.h>
#include <qdb/plan/ops/operator.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace NQdb {

enum class EExecPlanNodeKind {
    Source,
    Filter,
    Project,
    Aggregate,
    Join,
    CrossJoin,
    CrossResidualFilter,
    CteProducer,
    CteConsumer,
    Window,
    Sort,
    TopSort,
    Limit,
    UnionAll,
};

inline std::string_view ExecPlanNodeKindName(EExecPlanNodeKind kind) {
    switch (kind) {
        case EExecPlanNodeKind::Source: return "source";
        case EExecPlanNodeKind::Filter: return "filter";
        case EExecPlanNodeKind::Project: return "project";
        case EExecPlanNodeKind::Aggregate: return "aggregate";
        case EExecPlanNodeKind::Join: return "join";
        case EExecPlanNodeKind::CrossJoin: return "cross-join";
        case EExecPlanNodeKind::CrossResidualFilter:
            return "cross-residual-filter";
        case EExecPlanNodeKind::CteProducer: return "cte-producer";
        case EExecPlanNodeKind::CteConsumer: return "cte-consumer";
        case EExecPlanNodeKind::Window: return "window";
        case EExecPlanNodeKind::Sort: return "sort";
        case EExecPlanNodeKind::TopSort: return "top-sort";
        case EExecPlanNodeKind::Limit: return "limit";
        case EExecPlanNodeKind::UnionAll: return "union-all";
    }
    return "unknown";
}

// Neutral metadata emitted by scheduler lowering. OperatorOwner is populated
// only for synthetic operators that do not belong to the logical-plan tree.
struct TLoweredExecStage {
    TExecStageId Id = InvalidExecStageId;
    EExecPlanNodeKind Kind = EExecPlanNodeKind::Source;
    const IOperator* Operator = nullptr;
    TOperatorPtr OperatorOwner;
};

using TExecPlanNodeId = size_t;
inline constexpr TExecPlanNodeId InvalidExecPlanNodeId =
    std::numeric_limits<TExecPlanNodeId>::max();

struct TExecPlanNode {
    TExecPlanNodeId Id = InvalidExecPlanNodeId;
    TExecStageId StageId = InvalidExecStageId;
    EExecPlanNodeKind Kind = EExecPlanNodeKind::Source;
    const IOperator* Operator = nullptr;
    TOperatorPtr OperatorOwner;
    std::vector<TExecPlanNodeId> Inputs;
    NQumir::NAst::TTypePtr OutputType;
    std::vector<size_t> KernelIndexes;
    std::optional<size_t> MaterializationId;
    std::optional<std::vector<int32_t>> KeptInputColumns;
};

struct TExecPlanLimit {
    TExecStageId StageId = InvalidExecStageId;
    int64_t Limit = 0;
    int64_t Offset = 0;
};

// Exporter-neutral semantic projection of the graph that the scheduler will
// execute. Physical lanes and scheduler-only routing nodes are contracted.
struct TExecPlan {
    std::vector<TExecPlanNode> Nodes;
    TExecPlanNodeId Root = InvalidExecPlanNodeId;
    std::optional<TExecPlanLimit> RootLimit;
};

} // namespace NQdb
