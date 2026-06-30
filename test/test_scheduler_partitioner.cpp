#include <gtest/gtest.h>

#include <qdb/scheduler/partitioner.h>
#include <qdb/scheduler/single_threaded_scheduler.h>
#include <qdb/scheduler/threaded_scheduler.h>

#include <atomic>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace NQdb;
using namespace NQdb::NScheduler;

namespace {

struct TSourceState {
    size_t Partition = 0;
    int Next = 1;
    int End = 2;
    std::atomic<int>* DestroyCount = nullptr;
};

struct TUnaryState {
    int Calls = 0;
    int Add = 100;
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

struct TSpecFixture {
    TPipelinePartitionSpec Spec;
    std::shared_ptr<TSourceCode> SourceCode;
    std::shared_ptr<TUnaryCode> UnaryCode;
    std::shared_ptr<TSinkCode> SinkCode;
    std::vector<std::shared_ptr<TSourceState>> SourceStates;
    std::vector<std::shared_ptr<TUnaryState>> UnaryStates;
    std::shared_ptr<TSinkState> SinkState;
    std::shared_ptr<std::atomic<int>> DestroyCount;
};

TSpecFixture MakeSpec(size_t partitions) {
    TSpecFixture out;
    out.DestroyCount = std::make_shared<std::atomic<int>>(0);
    out.SinkState = std::make_shared<TSinkState>();
    out.SourceCode = std::make_shared<TSourceCode>(
        [](void* state, TRowSet& rowSet) {
            auto* source = static_cast<TSourceState*>(state);
            if (source->Next > source->End) {
                return false;
            }
            rowSet = MakeRowSet(
                static_cast<int64_t>(source->Partition * 10 + source->Next),
                source->DestroyCount);
            ++source->Next;
            return true;
        });
    out.UnaryCode = std::make_shared<TUnaryCode>(
        [](void* state, TRowSet& rowSet) {
            auto* unary = static_cast<TUnaryState*>(state);
            ++unary->Calls;
            rowSet.RowCount += unary->Add;
        });
    out.SinkCode = std::make_shared<TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            auto* sink = static_cast<TSinkState*>(state);
            sink->Rows += static_cast<int>(rowSet.RowCount);
            ++sink->Batches;
        });

    out.Spec.Source = TSourcePartitionSpec{
        .Code = out.SourceCode,
        .MakeState = [&out](size_t partition) {
            auto state = std::make_shared<TSourceState>(TSourceState{
                .Partition = partition,
                .DestroyCount = out.DestroyCount.get(),
            });
            out.SourceStates.push_back(state);
            return state;
        },
    };
    out.Spec.UnaryStages.push_back(TUnaryPartitionSpec{
        .Code = out.UnaryCode,
        .MakeState = [&out](size_t) {
            auto state = std::make_shared<TUnaryState>();
            out.UnaryStates.push_back(state);
            return state;
        },
    });
    out.Spec.Sink = TSinkPartitionSpec{
        .Code = out.SinkCode,
        .MakeState = [&out]() {
            return out.SinkState;
        },
    };
    out.Spec.Settings.Partitioning.DefaultPartitionCount = partitions;
    out.Spec.Settings.Partitioning.MaxPartitionCount = partitions;
    out.Spec.Settings.Queue.RowsetCapacityPerLane = 1;
    return out;
}

int ExpectedRows(size_t partitions) {
    int rows = 0;
    for (size_t partition = 0; partition < partitions; ++partition) {
        rows += static_cast<int>(partition * 10 + 1 + 100);
        rows += static_cast<int>(partition * 10 + 2 + 100);
    }
    return rows;
}

TEST(SchedulerPartitioner, BuildsSinglePartitionPipeline) {
    auto fixture = MakeSpec(1);
    std::string error;
    auto partitioned = TPipelinePartitioner::Build(fixture.Spec, &error);
    ASSERT_NE(partitioned.Graph, nullptr) << error;
    EXPECT_EQ(partitioned.Graph->Leaves().size(), 1u);
    EXPECT_EQ(partitioned.Graph->Edges().size(), 2u);

    TSingleThreadedScheduler scheduler(*partitioned.Graph);
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(fixture.SinkState->Batches, 2);
    EXPECT_EQ(fixture.SinkState->Rows, ExpectedRows(1));
    EXPECT_EQ(fixture.DestroyCount->load(std::memory_order_relaxed), 2);
}

TEST(SchedulerPartitioner, BuildsMultiPartitionPipeline) {
    constexpr size_t partitions = 3;

    auto fixture = MakeSpec(partitions);
    std::string error;
    auto partitioned = TPipelinePartitioner::Build(fixture.Spec, &error);
    ASSERT_NE(partitioned.Graph, nullptr) << error;
    EXPECT_EQ(partitioned.Graph->Leaves().size(), partitions);
    EXPECT_EQ(partitioned.Graph->Edges().size(), partitions * 2);
    EXPECT_NE(partitioned.Debug.find("partition_count=3"), std::string::npos);
    EXPECT_NE(partitioned.Debug.find("edge Gather"), std::string::npos);

    TThreadedScheduler scheduler(*partitioned.Graph, 4);
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(fixture.SinkState->Batches, static_cast<int>(partitions * 2));
    EXPECT_EQ(fixture.SinkState->Rows, ExpectedRows(partitions));
    EXPECT_EQ(
        fixture.DestroyCount->load(std::memory_order_relaxed),
        static_cast<int>(partitions * 2));
}

TEST(SchedulerPartitioner, SharesCodeAndCreatesPartitionLocalState) {
    constexpr size_t partitions = 4;

    auto fixture = MakeSpec(partitions);
    std::string error;
    auto partitioned = TPipelinePartitioner::Build(fixture.Spec, &error);
    ASSERT_NE(partitioned.Graph, nullptr) << error;
    EXPECT_EQ(fixture.SourceStates.size(), partitions);
    EXPECT_EQ(fixture.UnaryStates.size(), partitions);

    std::unordered_set<void*> sourceStates;
    std::unordered_set<void*> unaryStates;
    size_t sourceCount = 0;
    size_t unaryCount = 0;
    size_t sinkCount = 0;
    for (auto& node : partitioned.Graph->Nodes()) {
        if (auto* source = dynamic_cast<TSourceTask*>(node->Task)) {
            ++sourceCount;
            EXPECT_EQ(source->Code().get(), fixture.SourceCode.get());
            sourceStates.insert(source->State().get());
        } else if (auto* unary = dynamic_cast<TUnaryTask*>(node->Task)) {
            ++unaryCount;
            EXPECT_EQ(unary->Code().get(), fixture.UnaryCode.get());
            unaryStates.insert(unary->State().get());
        } else if (auto* sink = dynamic_cast<TSinkTask*>(node->Task)) {
            ++sinkCount;
            EXPECT_EQ(sink->Code().get(), fixture.SinkCode.get());
            EXPECT_EQ(sink->State().get(), fixture.SinkState.get());
        }
    }

    EXPECT_EQ(sourceCount, partitions);
    EXPECT_EQ(unaryCount, partitions);
    EXPECT_EQ(sinkCount, 1u);
    EXPECT_EQ(sourceStates.size(), partitions);
    EXPECT_EQ(unaryStates.size(), partitions);
}

TEST(SchedulerPartitioner, AppliesPartitionSettingsCap) {
    auto fixture = MakeSpec(8);
    fixture.Spec.Settings.Partitioning.MaxPartitionCount = 2;

    std::string error;
    auto partitioned = TPipelinePartitioner::Build(fixture.Spec, &error);
    ASSERT_NE(partitioned.Graph, nullptr) << error;
    EXPECT_EQ(partitioned.Graph->Leaves().size(), 2u);
    EXPECT_NE(partitioned.Debug.find("partition_count=2"), std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
