#pragma once

#include <qdb/io/io.h>
#include <qdb/kernel/generated.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/settings.h>

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace NQdb {
namespace NScheduler {

// A plan lowered into a scheduler task graph, without a terminal sink attached.
// FinalGather and Producers point into Graph; a sink is attached to FinalGather
// (one edge per producer lane) before the graph is run.
//
// Kernels holds every kernel generated during lowering, as ASTs with unbound
// slots (joined to graph nodes by Stage == DebugGroup). A finalizer must run
// before execution: RunPlanIntoSink JIT-binds them; the plan exporter compiles
// them to wasm instead.
struct TLoweredPlan {
    std::unique_ptr<TTaskGraph> Graph;
    NQumir::NAst::TTypePtr OutputType;
    IConnection* FinalGather = nullptr;
    std::vector<TTaskNode*> Producers;
    size_t Lanes = 0;
    std::vector<TGeneratedKernel> Kernels;
};

// Lower a logical plan into a scheduler graph (no terminal sink yet).
// Kernels are generated but not compiled.
TLoweredPlan LowerPlanToGraph(
    const TOperatorPtr& root,
    TSettings settings,
    std::ostream* diagnostics);

// Attach a terminal sink that writes every output rowset to `sink`, then run the
// graph on the configured scheduler. Returns false and sets `error` on failure.
bool RunPlanIntoSink(
    TLoweredPlan lowered,
    ISink& sink,
    TSettings settings,
    std::ostream* diagnostics,
    std::string* error);

} // namespace NScheduler
} // namespace NQdb
