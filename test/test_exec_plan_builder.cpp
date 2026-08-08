#include <gtest/gtest.h>

#include "mock_source.h"

#include <qdb/exec/plan_builder.h>
#include <qdb/plan/build.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/pipeline.h>
#include <qdb/scheduler/scan_split.h>
#include <qdb/sql/parser.h>

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <algorithm>
#include <expected>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

using namespace NQdb;

namespace {

class TSplitMockSource final
    : public TMockSource
    , public NScheduler::IScanMetadataSource
{
public:
    using TMockSource::TMockSource;

    std::vector<NScheduler::TScanRowGroup> ScanRowGroups() const override {
        return {
            {.RowGroup = 0, .RowCount = 100, .ByteSize = 800},
            {.RowGroup = 1, .RowCount = 100, .ByteSize = 800},
            {.RowGroup = 2, .RowCount = 100, .ByteSize = 800},
            {.RowGroup = 3, .RowCount = 100, .ByteSize = 800},
        };
    }
};

TOperatorPtr BuildSqlPlan(
    std::string_view sql,
    const std::unordered_map<std::string, ISource*>& sources)
{
    std::istringstream input{std::string(sql)};
    NSql::TTokenStream tokens(input);
    NSql::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    auto plan = BuildPlan(*parsed, [&](std::string_view table)
        -> std::expected<TOperatorPtr, NQumir::TError>
    {
        auto it = sources.find(std::string(table));
        if (it == sources.end()) {
            return std::unexpected(
                NQumir::TError("unknown table: " + std::string(table)));
        }
        return std::make_shared<TSourceOperator>(
            *it->second, std::string(table));
    });
    if (!plan) {
        throw std::runtime_error(plan.error().ToString());
    }
    auto root = *plan;
    ApplyPlanPasses(root, {.EnableCbo = false});
    return root;
}

NScheduler::TSettings SingleSettings() {
    NScheduler::TSettings settings;
    settings.Scheduler.Mode =
        NScheduler::EExecutionMode::SingleThreadedScheduler;
    settings.Scheduler.WorkerCount = 1;
    settings.ScanSplit.Strategy = NScheduler::EScanSplitStrategy::SerialRead;
    settings.ScanSplit.MaxScanTasks = 1;
    settings.HashShuffle.PartitionCount = 1;
    settings.HashShuffle.MaxPartitionCount = 1;
    return settings;
}

NScheduler::TSettings MultiSettings() {
    NScheduler::TSettings settings;
    settings.Scheduler.Mode =
        NScheduler::EExecutionMode::ThreadedScheduler;
    settings.Scheduler.WorkerCount = 4;
    settings.ScanSplit.Strategy =
        NScheduler::EScanSplitStrategy::RowGroupRange;
    settings.ScanSplit.MaxScanTasks = 4;
    settings.HashShuffle.PartitionCount = 2;
    settings.HashShuffle.MaxPartitionCount = 2;
    return settings;
}

struct TBuiltPlan {
    TOperatorPtr Logical;
    TExecPlan Exec;
    std::vector<TGeneratedKernel> Kernels;
    size_t PhysicalTasks = 0;
};

TBuiltPlan Lower(
    std::string_view sql,
    const std::unordered_map<std::string, ISource*>& sources,
    NScheduler::TSettings settings)
{
    auto logical = BuildSqlPlan(sql, sources);
    auto lowered = NScheduler::LowerPlanToGraph(logical, settings, nullptr);
    auto exec = BuildExecPlan(lowered);
    if (!exec) {
        throw std::runtime_error(exec.error());
    }
    return {
        .Logical = std::move(logical),
        .Exec = std::move(*exec),
        .Kernels = std::move(lowered.Kernels),
        .PhysicalTasks = lowered.Graph->Nodes().size(),
    };
}

void ExpectSameSemanticPlan(const TExecPlan& left, const TExecPlan& right) {
    ASSERT_EQ(left.Root, right.Root);
    ASSERT_EQ(left.Nodes.size(), right.Nodes.size());
    ASSERT_EQ(left.RootLimit.has_value(), right.RootLimit.has_value());
    for (size_t i = 0; i < left.Nodes.size(); ++i) {
        const auto& l = left.Nodes[i];
        const auto& r = right.Nodes[i];
        EXPECT_EQ(l.Id, r.Id) << i;
        EXPECT_EQ(l.StageId, r.StageId) << i;
        EXPECT_EQ(l.Kind, r.Kind) << i;
        EXPECT_EQ(l.Inputs, r.Inputs) << i;
        ASSERT_TRUE(l.OutputType) << i;
        ASSERT_TRUE(r.OutputType) << i;
        EXPECT_EQ(l.OutputType->ToString(), r.OutputType->ToString()) << i;
    }
}

void ExpectKernelBindings(const TBuiltPlan& built) {
    std::vector<bool> seen(built.Kernels.size(), false);
    for (const auto& node : built.Exec.Nodes) {
        for (const auto kernelIndex : node.KernelIndexes) {
            ASSERT_LT(kernelIndex, built.Kernels.size());
            EXPECT_EQ(built.Kernels[kernelIndex].ExecStageId, node.StageId);
            seen[kernelIndex] = true;
        }
    }
    EXPECT_TRUE(std::ranges::all_of(seen, [](bool value) { return value; }));
}

size_t CountKind(const TExecPlan& plan, EExecPlanNodeKind kind) {
    return std::ranges::count_if(plan.Nodes, [&](const auto& node) {
        return node.Kind == kind;
    });
}

TEST(ExecPlanBuilder, SingleAndMultiCollapseJoinToSameSemanticDag) {
    TSplitMockSource left({"k", "lv"});
    TSplitMockSource right({"k", "rv"});
    const std::unordered_map<std::string, ISource*> sources{
        {"l", &left},
        {"r", &right},
    };
    constexpr std::string_view sql =
        "SELECT l.k FROM l JOIN r ON l.k = r.k";

    auto single = Lower(sql, sources, SingleSettings());
    auto multi = Lower(sql, sources, MultiSettings());

    EXPECT_GT(multi.PhysicalTasks, single.PhysicalTasks);
    ExpectSameSemanticPlan(single.Exec, multi.Exec);
    ExpectKernelBindings(single);
    ExpectKernelBindings(multi);
    EXPECT_EQ(CountKind(single.Exec, EExecPlanNodeKind::Join), 1u);
    EXPECT_EQ(CountKind(multi.Exec, EExecPlanNodeKind::Join), 1u);
}

TEST(ExecPlanBuilder, SingleAndMultiCollapseUnionLanes) {
    TSplitMockSource left({"k"});
    TSplitMockSource right({"k"});
    const std::unordered_map<std::string, ISource*> sources{
        {"l", &left},
        {"r", &right},
    };
    constexpr std::string_view sql =
        "SELECT k FROM l UNION ALL SELECT k FROM r";

    auto single = Lower(sql, sources, SingleSettings());
    auto multi = Lower(sql, sources, MultiSettings());

    EXPECT_GT(multi.PhysicalTasks, single.PhysicalTasks);
    ExpectSameSemanticPlan(single.Exec, multi.Exec);
    ExpectKernelBindings(single);
    ExpectKernelBindings(multi);
    ASSERT_EQ(CountKind(single.Exec, EExecPlanNodeKind::UnionAll), 1u);
    const auto& root = single.Exec.Nodes[single.Exec.Root];
    EXPECT_EQ(root.Kind, EExecPlanNodeKind::UnionAll);
    EXPECT_EQ(root.StageId, InvalidExecStageId);
    EXPECT_EQ(root.Inputs.size(), 2u);
}

TEST(ExecPlanBuilder, MultiKeepsRealPartialAndCombineStages) {
    TSplitMockSource source({"k"});
    const std::unordered_map<std::string, ISource*> sources{{"t", &source}};
    constexpr std::string_view sql = "SELECT count(*) AS c FROM t";

    auto singleSettings = SingleSettings();
    singleSettings.Aggregate.CascadeGlobal = true;
    auto multiSettings = MultiSettings();
    multiSettings.Aggregate.CascadeGlobal = true;

    auto single = Lower(sql, sources, singleSettings);
    auto multi = Lower(sql, sources, multiSettings);

    EXPECT_EQ(CountKind(single.Exec, EExecPlanNodeKind::Aggregate), 1u);
    EXPECT_EQ(CountKind(multi.Exec, EExecPlanNodeKind::Aggregate), 2u);
    EXPECT_GT(multi.PhysicalTasks, single.PhysicalTasks);
    ExpectKernelBindings(single);
    ExpectKernelBindings(multi);
}

TEST(ExecPlanBuilder, CteConsumersShareTypedMaterialization) {
    TSplitMockSource source({"k", "v"});
    const std::unordered_map<std::string, ISource*> sources{{"t", &source}};
    constexpr std::string_view sql =
        "WITH x AS (SELECT k, v FROM t) "
        "SELECT p.k, q.v FROM x p JOIN x q ON p.k = q.k";

    auto built = Lower(sql, sources, MultiSettings());

    ASSERT_EQ(CountKind(built.Exec, EExecPlanNodeKind::CteProducer), 1u);
    ASSERT_EQ(CountKind(built.Exec, EExecPlanNodeKind::CteConsumer), 2u);
    std::optional<size_t> materialization;
    for (const auto& node : built.Exec.Nodes) {
        if (node.Kind != EExecPlanNodeKind::CteProducer &&
            node.Kind != EExecPlanNodeKind::CteConsumer)
        {
            continue;
        }
        ASSERT_TRUE(node.MaterializationId);
        if (!materialization) {
            materialization = node.MaterializationId;
        }
        EXPECT_EQ(node.MaterializationId, materialization);
    }
    ExpectKernelBindings(built);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
