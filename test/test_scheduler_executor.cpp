#include <gtest/gtest.h>

#include <qdb/scheduler/executor.h>
#include <qdb/scheduler/partitioner.h>

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

struct TSinkState {
    int Rows = 0;
    int Batches = 0;
};

void CountDestroy(TRowSet* rowSet) {
    auto* counter = static_cast<std::atomic<int>*>(rowSet->Private);
    counter->fetch_add(1, std::memory_order_relaxed);
}

TRowSet MakeRowSet(int64_t rows, std::atomic<int>* destroyCount) {
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

struct TFixture {
    TPipelinePartitionSpec Spec;
    std::shared_ptr<TSinkState> SinkState;
    std::shared_ptr<std::atomic<int>> DestroyCount;
};

TFixture MakeSpec(EExecutionMode mode) {
    TFixture out;
    out.SinkState = std::make_shared<TSinkState>();
    out.DestroyCount = std::make_shared<std::atomic<int>>(0);

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
    auto sinkCode = std::make_shared<TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            auto* sink = static_cast<TSinkState*>(state);
            sink->Rows += static_cast<int>(rowSet.RowCount);
            ++sink->Batches;
        });

    out.Spec.Source = TSourcePartitionSpec{
        .Code = sourceCode,
        .MakeState = [&out](size_t, const TScanSplit*) {
            return std::make_shared<TSourceState>(TSourceState{
                .DestroyCount = out.DestroyCount.get(),
            });
        },
    };
    out.Spec.Sink = TSinkPartitionSpec{
        .Code = sinkCode,
        .MakeState = [&out]() {
            return out.SinkState;
        },
    };
    out.Spec.Settings.Scheduler.Mode = mode;
    out.Spec.Settings.Scheduler.WorkerCount = 2;
    out.Spec.Settings.Scheduler.ReadyQueueCapacity = 16;
    out.Spec.Settings.Partitioning.DefaultPartitionCount = 2;
    out.Spec.Settings.Partitioning.MaxPartitionCount = 2;
    return out;
}

TEST(SchedulerExecutor, RejectsSerialModeForSchedulerGraph) {
    auto fixture = MakeSpec(EExecutionMode::Serial);
    std::string error;
    auto partitioned = TPipelinePartitioner::Build(fixture.Spec, &error);
    ASSERT_NE(partitioned.Graph, nullptr) << error;

    TSchedulerExecutor executor(*partitioned.Graph, fixture.Spec.Settings);
    EXPECT_FALSE(executor.Run(&error));
    EXPECT_NE(error.find("current physical executor"), std::string::npos);
    EXPECT_EQ(fixture.SinkState->Batches, 0);
}

TEST(SchedulerExecutor, RunsSingleThreadedSchedulerMode) {
    auto fixture = MakeSpec(EExecutionMode::SingleThreadedScheduler);
    std::string error;
    auto partitioned = TPipelinePartitioner::Build(fixture.Spec, &error);
    ASSERT_NE(partitioned.Graph, nullptr) << error;

    TSchedulerExecutor executor(*partitioned.Graph, fixture.Spec.Settings);
    ASSERT_TRUE(executor.Run(&error)) << error;
    EXPECT_EQ(fixture.SinkState->Batches, 4);
    EXPECT_EQ(fixture.SinkState->Rows, 6);
    EXPECT_EQ(fixture.DestroyCount->load(std::memory_order_relaxed), 4);
}

TEST(SchedulerExecutor, RunsThreadedSchedulerMode) {
    auto fixture = MakeSpec(EExecutionMode::ThreadedScheduler);
    std::string error;
    auto partitioned = TPipelinePartitioner::Build(fixture.Spec, &error);
    ASSERT_NE(partitioned.Graph, nullptr) << error;

    TSchedulerExecutor executor(*partitioned.Graph, fixture.Spec.Settings);
    ASSERT_TRUE(executor.Run(&error)) << error;
    EXPECT_EQ(fixture.SinkState->Batches, 4);
    EXPECT_EQ(fixture.SinkState->Rows, 6);
    EXPECT_EQ(fixture.DestroyCount->load(std::memory_order_relaxed), 4);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
