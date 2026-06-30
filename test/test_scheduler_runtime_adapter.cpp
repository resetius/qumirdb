#include <gtest/gtest.h>

#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/runtime_adapter.h>
#include <qdb/scheduler/single_threaded_scheduler.h>
#include <qdb/scheduler/threaded_scheduler.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace NQdb;
using namespace NQdb::NScheduler;

namespace {

struct TSourceState {
    int Next = 0;
    int End = 0;
};

struct TUnaryState {
    int Calls = 0;
    int Add = 0;
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

struct TPipeline {
    std::unique_ptr<TTaskGraph> Graph;
    std::shared_ptr<TSourceState> SourceState;
    std::shared_ptr<TUnaryState> FilterState;
    std::shared_ptr<TUnaryState> ProjectState;
    std::shared_ptr<TSinkState> SinkState;
    std::shared_ptr<TSourceCode> SourceCode;
    std::shared_ptr<TUnaryCode> FilterCode;
    std::shared_ptr<TUnaryCode> ProjectCode;
    std::shared_ptr<TSinkCode> SinkCode;
    std::shared_ptr<std::atomic<int>> DestroyCount;
};

TPipeline MakePipeline() {
    TPipeline out;
    out.Graph = std::make_unique<TTaskGraph>();
    out.SourceState = std::make_shared<TSourceState>(TSourceState{
        .Next = 1,
        .End = 4,
    });
    out.FilterState = std::make_shared<TUnaryState>(TUnaryState{
        .Add = 10,
    });
    out.ProjectState = std::make_shared<TUnaryState>(TUnaryState{
        .Add = 100,
    });
    out.SinkState = std::make_shared<TSinkState>();
    out.DestroyCount = std::make_shared<std::atomic<int>>(0);

    out.SourceCode = std::make_shared<TSourceCode>(
        [destroyCount = out.DestroyCount](void* state, TRowSet& rowSet) {
            auto* source = static_cast<TSourceState*>(state);
            if (source->Next > source->End) {
                return false;
            }
            rowSet = MakeRowSet(source->Next, destroyCount.get());
            ++source->Next;
            return true;
        });
    out.FilterCode = std::make_shared<TUnaryCode>(
        [](void* state, TRowSet& rowSet) {
            auto* unary = static_cast<TUnaryState*>(state);
            ++unary->Calls;
            rowSet.RowCount += unary->Add;
        });
    out.ProjectCode = std::make_shared<TUnaryCode>(
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

    auto sourceToFilter = std::make_unique<TOneToOneConnection>();
    auto* sourceToFilterPtr = sourceToFilter.get();
    auto filterToProject = std::make_unique<TOneToOneConnection>();
    auto* filterToProjectPtr = filterToProject.get();
    auto projectToSink = std::make_unique<TOneToOneConnection>();
    auto* projectToSinkPtr = projectToSink.get();

    auto& source = out.Graph->AddOwnedNode(std::make_unique<TSourceTask>(
        out.SourceCode,
        out.SourceState,
        TOutputPort{.Connection = sourceToFilterPtr}));
    auto& filter = out.Graph->AddOwnedNode(std::make_unique<TUnaryTask>(
        out.FilterCode,
        out.FilterState,
        TInputPort{.Connection = sourceToFilterPtr},
        TOutputPort{.Connection = filterToProjectPtr}));
    auto& project = out.Graph->AddOwnedNode(std::make_unique<TUnaryTask>(
        out.ProjectCode,
        out.ProjectState,
        TInputPort{.Connection = filterToProjectPtr},
        TOutputPort{.Connection = projectToSinkPtr}));
    auto& sink = out.Graph->AddOwnedNode(std::make_unique<TSinkTask>(
        out.SinkCode,
        out.SinkState,
        TInputPort{.Connection = projectToSinkPtr}));

    out.Graph->AddOwnedEdge(source, filter, std::move(sourceToFilter));
    out.Graph->AddOwnedEdge(filter, project, std::move(filterToProject));
    out.Graph->AddOwnedEdge(project, sink, std::move(projectToSink));
    out.Graph->Build();
    return out;
}

TEST(SchedulerRuntimeAdapter, RunsSourceFilterProjectSinkSingleThreaded) {
    auto pipeline = MakePipeline();
    TSingleThreadedScheduler scheduler(*pipeline.Graph);

    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(pipeline.FilterState->Calls, 4);
    EXPECT_EQ(pipeline.ProjectState->Calls, 4);
    EXPECT_EQ(pipeline.SinkState->Batches, 4);
    EXPECT_EQ(pipeline.SinkState->Rows, 450);
    EXPECT_EQ(pipeline.DestroyCount->load(std::memory_order_relaxed), 4);
}

TEST(SchedulerRuntimeAdapter, RunsSourceFilterProjectSinkThreaded) {
    auto pipeline = MakePipeline();
    TThreadedScheduler scheduler(*pipeline.Graph, 4);

    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(pipeline.FilterState->Calls, 4);
    EXPECT_EQ(pipeline.ProjectState->Calls, 4);
    EXPECT_EQ(pipeline.SinkState->Batches, 4);
    EXPECT_EQ(pipeline.SinkState->Rows, 450);
    EXPECT_EQ(pipeline.DestroyCount->load(std::memory_order_relaxed), 4);
}

TEST(SchedulerRuntimeAdapter, PartitionTasksShareCodeButOwnState) {
    auto code = std::make_shared<TUnaryCode>(
        [](void* state, TRowSet& rowSet) {
            auto* unary = static_cast<TUnaryState*>(state);
            ++unary->Calls;
            rowSet.RowCount += unary->Add;
        });
    auto leftState = std::make_shared<TUnaryState>(TUnaryState{
        .Add = 1,
    });
    auto rightState = std::make_shared<TUnaryState>(TUnaryState{
        .Add = 2,
    });

    TUnaryTask left(
        code,
        leftState,
        TInputPort{},
        TOutputPort{});
    TUnaryTask right(
        code,
        rightState,
        TInputPort{},
        TOutputPort{});

    EXPECT_EQ(left.Code().get(), right.Code().get());
    EXPECT_NE(left.State().get(), right.State().get());
    EXPECT_EQ(code.use_count(), 3);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
