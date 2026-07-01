#include <qdb/exec/planner.h>
#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/join_exec.h>
#include <qdb/exec/planner_helpers.h>
#include <qdb/exec/sort_exec.h>
#include <qdb/exec/source_exec.h>
#include <qdb/exec/unary_block_exec.h>
#include <qdb/exec/unary_stream_exec.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/scheduler/plan_lowerer.h>

#include <stdexcept>

namespace NQdb {

void TPhysicalPlanner::PrintRuntimePlan(const TOperatorPtr& root) const {
    if (!Diagnostics_) {
        return;
    }
    *Diagnostics_ << "\n========== RUNTIME PLAN ==========\n";
    PrintRuntimePlan(root, 0);
    *Diagnostics_ << "==================================\n";
}

void TPhysicalPlanner::PrintRuntimePlan(const TOperatorPtr& root, int depth) const {
    const std::string indent(static_cast<size_t>(depth) * 2, ' ');
    *Diagnostics_ << indent;
    if (auto node = TMaybeOp<TSourceOperator>(root)) {
        *Diagnostics_ << "source " << node.Cast()->SourcePath() << "\n";
        return;
    }
    if (auto node = TMaybeOp<TFilterOperator>(root)) {
        *Diagnostics_ << "filter [JIT: AST -> IR -> LLVM]\n";
        PrintRuntimePlan(node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TProjectOperator>(root)) {
        *Diagnostics_ << "project [column mapping]\n";
        PrintRuntimePlan(node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TAggregateOperator>(root)) {
        *Diagnostics_ << "aggregate [JIT: update + finalize]\n";
        PrintRuntimePlan(node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TJoinOperator>(root)) {
        *Diagnostics_ << "join [symmetric hash, JIT probe+insert]\n";
        PrintRuntimePlan(node.Cast()->Left(), depth + 1);
        PrintRuntimePlan(node.Cast()->Right(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TSortOperator>(root)) {
        *Diagnostics_ << "sort [stable indices]\n";
        PrintRuntimePlan(node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TTopSortOperator>(root)) {
        *Diagnostics_ << "top-sort [bounded stable state]\n";
        PrintRuntimePlan(node.Cast()->Input(), depth + 1);
        return;
    }
    if (auto node = TMaybeOp<TLimitOperator>(root)) {
        *Diagnostics_ << "limit [" << node.Cast()->Limit()
            << ", offset " << node.Cast()->Offset() << "]\n";
        PrintRuntimePlan(node.Cast()->Input(), depth + 1);
        return;
    }
    *Diagnostics_ << "unknown\n";
}

std::unique_ptr<IRuntimeNode> TPhysicalPlanner::Build(const TOperatorPtr& root) {
    if (SchedulerSettings_.Scheduler.Mode != NScheduler::EExecutionMode::Serial) {
        // Lower the whole plan into a single scheduler graph. Returns null when
        // the plan contains an operator the lowering does not yet support, in
        // which case we fall through to the serial builders below.
        if (auto runtime = NScheduler::BuildSchedulerPlanPipeline(
                root,
                SchedulerSettings_,
                Diagnostics_))
        {
            return runtime;
        }
    }

    if (auto maybe = TMaybeOp<TSourceOperator>(root)) {
        auto src = maybe.Cast();
        return std::make_unique<TRuntimeSource>(
            src->GetSource(),
            BuildSourceRuntimeType(*src));
    }

    if (auto maybe = TMaybeOp<TFilterOperator>(root)) {
        auto filter = maybe.Cast();
        auto input = Build(filter->Input());
        auto runtime = BuildFilterRuntimeProcess(
            *filter,
            input->OutputType(),
            Diagnostics_);
        return std::make_unique<TRuntimeUnaryStreamingKernel>(
            std::move(input),
            std::move(runtime.OutputType),
            std::move(runtime.Process));
    }

    if (auto maybe = TMaybeOp<TProjectOperator>(root)) {
        auto project = maybe.Cast();
        auto input = Build(project->Input());
        auto runtime = BuildProjectRuntimeProcess(
            *project,
            input->OutputType(),
            Diagnostics_);
        return std::make_unique<TRuntimeUnaryStreamingKernel>(
            std::move(input),
            std::move(runtime.OutputType),
            std::move(runtime.Process));
    }

    if (auto maybe = TMaybeOp<TAggregateOperator>(root)) {
        auto agg = maybe.Cast();
        auto input = Build(agg->Input());
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(input->OutputType().get());
        if (!inputType) {
            throw std::runtime_error("aggregate input must have TStructType");
        }

        auto spec = NKernel::BuildAggregateKernelSpec(
            *inputType, agg->GroupKeys(), agg->Aggs());
        TKernelCompiler compiler(Diagnostics_);
        auto kernels = compiler.CompileAggregate(spec);

        // Output type from the physical (pruned) input type, not the logical
        // OutputColumns() (which was computed from the pre-pruning schema).
        auto outputType = ComputeAggregateOutputType(input->OutputType(), agg->GroupKeys(), agg->Aggs());

        return std::make_unique<TRuntimeUnaryBlockingKernel>(
            std::move(input),
            std::move(outputType),
            MakeAggregateProcess(std::move(kernels)));
    }

    if (auto maybe = TMaybeOp<TJoinOperator>(root)) {
        using namespace NQumir::NAst;
        auto join = maybe.Cast();
        auto left = Build(join->Left());
        auto right = Build(join->Right());
        auto* leftType = static_cast<TStructType*>(left->OutputType().get());
        auto* rightType = static_cast<TStructType*>(right->OutputType().get());
        if (!leftType || !rightType) {
            throw std::runtime_error("join inputs must have TStructType");
        }

        // Cross join: no key columns → Cartesian product executor.
        if (join->Keys().empty()) {
            auto outputType = ComputeJoinOutputType(
                left->OutputType(), right->OutputType(), join->JoinType());
            if (!outputType) {
                throw std::runtime_error("cross join: " + outputType.error().ToString());
            }
            return std::make_unique<TRuntimeCrossJoin>(
                std::move(left), std::move(right), std::move(*outputType));
        }

        auto spec = NKernel::BuildJoinKernelSpec(
            *leftType, *rightType, join->Keys(), join->JoinType(), join->Filter());

        TKernelCompiler compiler(Diagnostics_);
        auto kernels = compiler.CompileJoin(spec);

        return std::make_unique<TRuntimeJoin>(
            std::move(left), std::move(right), std::move(spec.OutputSchema),
            std::move(kernels),
            join->JoinType(),
            /*hasResidual=*/join->Filter() != nullptr);
    }

    if (auto maybe = TMaybeOp<TSortOperator>(root)) {
        auto sort = maybe.Cast();
        auto input = Build(sort->Input());
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(input->OutputType().get());
        if (!inputType) {
            throw std::runtime_error("sort input must have TStructType");
        }

        auto runtime = BuildSortRuntimeProcess(
            *inputType,
            sort->Keys(),
            "sort",
            Diagnostics_);

        auto outputType = input->OutputType();
        return std::make_unique<TRuntimeUnaryBlockingKernel>(
            std::move(input),
            outputType,
            MakeSortProcess(
                outputType,
                sort->Keys(),
                std::move(runtime.KeyColumns),
                std::move(runtime.RadixKernel)));
    }

    if (auto maybe = TMaybeOp<TTopSortOperator>(root)) {
        auto sort = maybe.Cast();
        auto input = Build(sort->Input());
        auto* inputType = static_cast<NQumir::NAst::TStructType*>(input->OutputType().get());
        if (!inputType) {
            throw std::runtime_error("top-sort input must have TStructType");
        }

        auto runtime = BuildSortRuntimeProcess(
            *inputType,
            sort->Keys(),
            "top-sort",
            Diagnostics_);

        auto outputType = input->OutputType();
        return std::make_unique<TRuntimeUnaryBlockingKernel>(
            std::move(input),
            outputType,
            MakeTopSortProcess(
                outputType,
                sort->Keys(),
                std::move(runtime.KeyColumns),
                std::move(runtime.RadixKernel),
                sort->Limit()));
    }

    if (auto maybe = TMaybeOp<TLimitOperator>(root)) {
        auto limit = maybe.Cast();
        auto input = Build(limit->Input());
        return std::make_unique<TRuntimeLimit>(
            std::move(input),
            input->OutputType(),
            limit->Limit(),
            limit->Offset());
    }

    throw std::runtime_error("TPhysicalPlanner: unknown operator");
}

} // namespace NQdb
