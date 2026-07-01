#include <gtest/gtest.h>

#include <qdb/scheduler/partitioner.h>
#include <qdb/scheduler/runtime_node.h>

#include <atomic>
#include <memory>
#include <string>

using namespace NQdb;
using namespace NQdb::NScheduler;

namespace {

struct TSourceState {
    int Next = 1;
    int End = 2;
    std::atomic<int>* DestroyCount = nullptr;
};

struct TUnaryState {
    int Add = 100;
};

void CountDestroy(TRowSet* rowSet)
{
    auto* counter = static_cast<std::atomic<int>*>(rowSet->Private);
    counter->fetch_add(1, std::memory_order_relaxed);
}

TRowSet MakeRowSet(int64_t rows, std::atomic<int>* destroyCount)
{
    return TRowSet{
        .Columns = nullptr,
        .ColumnCount = 0,
        .RowCount = rows,
        .Selection = nullptr,
        .Destroy = CountDestroy,
        .Private = destroyCount,
        .RefCount = 1,
    };
}

std::unique_ptr<IRuntimeNode> MakeRuntimeNode(
    std::shared_ptr<std::atomic<int>> destroyCount)
{
    auto output = std::make_shared<TBufferedSchedulerOutput>();
    auto sourceCode = std::make_shared<TSourceCode>(
        [](void* state, TRowSet& rowSet) {
            auto* source = static_cast<TSourceState*>(state);
            if (source->Next > source->End) {
                return false;
            }
            rowSet = MakeRowSet(source->Next, source->DestroyCount);
            ++source->Next;
            return true;
        });
    auto unaryCode = std::make_shared<TUnaryCode>(
        [](void* state, TRowSet& rowSet) {
            auto* unary = static_cast<TUnaryState*>(state);
            rowSet.RowCount += unary->Add;
        });

    TPipelinePartitionSpec spec;
    spec.Source = TSourcePartitionSpec{
        .Code = sourceCode,
        .MakeState = [destroyCount](size_t, const TScanSplit*) {
            return std::make_shared<TSourceState>(TSourceState{
                .DestroyCount = destroyCount.get(),
            });
        },
    };
    spec.UnaryStages.push_back(TUnaryPartitionSpec{
        .Code = unaryCode,
        .MakeState = [](size_t) {
            return std::make_shared<TUnaryState>();
        },
    });
    spec.Sink = TSinkPartitionSpec{
        .Code = MakeBufferedSchedulerSinkCode(),
        .MakeState = [output]() {
            return output;
        },
    };
    spec.Settings.Scheduler.Mode = EExecutionMode::ThreadedScheduler;
    spec.Settings.Scheduler.WorkerCount = 2;
    spec.Settings.Partitioning.DefaultPartitionCount = 2;
    spec.Settings.Partitioning.MaxPartitionCount = 2;

    std::string error;
    auto partitioned = TPipelinePartitioner::Build(spec, &error);
    EXPECT_NE(partitioned.Graph, nullptr) << error;

    return std::make_unique<TRuntimeSchedulerPipeline>(
        std::move(partitioned.Graph),
        spec.Settings,
        NQumir::NAst::TTypePtr{},
        output);
}

TEST(SchedulerRuntimeNode, RunsSchedulerGraphBehindRuntimeNode)
{
    auto destroyCount = std::make_shared<std::atomic<int>>(0);
    auto node = MakeRuntimeNode(destroyCount);

    int rows = 0;
    int batches = 0;
    TRowSet rowSet{};
    while (node->Next(rowSet)) {
        rows += static_cast<int>(rowSet.RowCount);
        ++batches;
        Release(&rowSet);
    }

    EXPECT_EQ(batches, 4);
    EXPECT_EQ(rows, 406);
    EXPECT_EQ(destroyCount->load(std::memory_order_relaxed), 4);
}

TEST(SchedulerRuntimeNode, ReleasesBufferedRowsOnDestroy)
{
    auto destroyCount = std::make_shared<std::atomic<int>>(0);
    {
        auto node = MakeRuntimeNode(destroyCount);
        TRowSet rowSet{};
        ASSERT_TRUE(node->Next(rowSet));
        Release(&rowSet);
    }

    EXPECT_EQ(destroyCount->load(std::memory_order_relaxed), 4);
}

} // namespace

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
