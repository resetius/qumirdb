#include <gtest/gtest.h>

#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/runtime_adapter.h>
#include <qdb/scheduler/single_threaded_scheduler.h>
#include <qdb/scheduler/threaded_scheduler.h>

#include <qumir/parser/type.h>

#include <atomic>
#include <cstdint>
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

struct TBinaryState {
    int LeftRows = 0;
    int RightRows = 0;
    bool LeftDone = false;
    bool RightDone = false;
    std::atomic<int>* DestroyCount = nullptr;
};

void CountDestroy(TRowSet* rowSet) {
    auto* counter = static_cast<std::atomic<int>*>(rowSet->Private);
    counter->fetch_add(1, std::memory_order_relaxed);
}

struct TInt64RowSetData {
    std::vector<int64_t> Values;
    std::vector<TColumn> Columns;
    std::atomic<int>* DestroyCount = nullptr;
};

void DestroyInt64RowSet(TRowSet* rowSet) {
    auto* data = static_cast<TInt64RowSetData*>(rowSet->Private);
    data->DestroyCount->fetch_add(1, std::memory_order_relaxed);
    delete data;
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

TRowSet MakeInt64RowSet(std::vector<int64_t> values,
    std::atomic<int>* destroyCount)
{
    auto* data = new TInt64RowSetData;
    data->Values = std::move(values);
    data->Columns.push_back(TColumn{
        .Data = reinterpret_cast<char*>(data->Values.data()),
        .Mask = nullptr,
        .Offsets = nullptr,
        .OffsetWidth = 0,
    });
    data->DestroyCount = destroyCount;
    return TRowSet{
        .Columns = data->Columns.data(),
        .ColumnCount = 1,
        .RowCount = static_cast<int64_t>(data->Values.size()),
        .Selection = nullptr,
        .Destroy = DestroyInt64RowSet,
        .Private = data,
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

TEST(SchedulerRuntimeAdapter, RunsBinaryBlockingTask) {
    auto graph = std::make_unique<TTaskGraph>();
    auto destroyCount = std::make_shared<std::atomic<int>>(0);
    auto leftState = std::make_shared<TSourceState>(TSourceState{
        .Next = 1,
        .End = 2,
    });
    auto rightState = std::make_shared<TSourceState>(TSourceState{
        .Next = 10,
        .End = 11,
    });
    auto binaryState = std::make_shared<TBinaryState>(TBinaryState{
        .DestroyCount = destroyCount.get(),
    });
    auto sinkState = std::make_shared<TSinkState>();

    auto sourceCode = std::make_shared<TSourceCode>(
        [destroyCount](void* state, TRowSet& rowSet) {
            auto* source = static_cast<TSourceState*>(state);
            if (source->Next > source->End) {
                return false;
            }
            rowSet = MakeRowSet(source->Next, destroyCount.get());
            ++source->Next;
            return true;
        });
    auto binaryCode = std::make_shared<TBinaryBlockingCode>(
        [](void* state, TInputPort& left, TInputPort& right, TRowSet& output) {
            auto* binary = static_cast<TBinaryState*>(state);
            TRowSet rowSet{};
            while (!binary->LeftDone) {
                auto fetch = left.Fetch(rowSet);
                if (fetch == EFetchResult::NO_DATA) {
                    break;
                }
                if (fetch == EFetchResult::FINISHED) {
                    binary->LeftDone = true;
                    break;
                }
                binary->LeftRows += static_cast<int>(rowSet.RowCount);
                Release(&rowSet);
                rowSet = {};
            }
            while (!binary->RightDone) {
                auto fetch = right.Fetch(rowSet);
                if (fetch == EFetchResult::NO_DATA) {
                    break;
                }
                if (fetch == EFetchResult::FINISHED) {
                    binary->RightDone = true;
                    break;
                }
                binary->RightRows += static_cast<int>(rowSet.RowCount);
                Release(&rowSet);
                rowSet = {};
            }
            if (!binary->LeftDone || !binary->RightDone) {
                return ETaskResult::NEED_DATA;
            }
            if (binary->DestroyCount == nullptr) {
                return ETaskResult::FINISHED;
            }
            output = MakeRowSet(
                binary->LeftRows * 100 + binary->RightRows,
                binary->DestroyCount);
            binary->DestroyCount = nullptr;
            return ETaskResult::OK;
        });
    auto sinkCode = std::make_shared<TSinkCode>(
        [](void* state, const TRowSet& rowSet) {
            auto* sink = static_cast<TSinkState*>(state);
            sink->Rows += static_cast<int>(rowSet.RowCount);
            ++sink->Batches;
        });

    auto leftToBinary = std::make_unique<TOneToOneConnection>();
    auto* leftToBinaryPtr = leftToBinary.get();
    auto rightToBinary = std::make_unique<TOneToOneConnection>();
    auto* rightToBinaryPtr = rightToBinary.get();
    auto binaryToSink = std::make_unique<TOneToOneConnection>();
    auto* binaryToSinkPtr = binaryToSink.get();

    auto& left = graph->AddOwnedNode(std::make_unique<TSourceTask>(
        sourceCode,
        leftState,
        TOutputPort{.Connection = leftToBinaryPtr}));
    auto& right = graph->AddOwnedNode(std::make_unique<TSourceTask>(
        sourceCode,
        rightState,
        TOutputPort{.Connection = rightToBinaryPtr}));
    auto& binary = graph->AddOwnedNode(std::make_unique<TBinaryBlockingTask>(
        binaryCode,
        binaryState,
        TInputPort{.Connection = leftToBinaryPtr},
        TInputPort{.Connection = rightToBinaryPtr},
        TOutputPort{.Connection = binaryToSinkPtr}));
    auto& sink = graph->AddOwnedNode(std::make_unique<TSinkTask>(
        sinkCode,
        sinkState,
        TInputPort{.Connection = binaryToSinkPtr}));

    graph->AddOwnedEdge(left, binary, std::move(leftToBinary));
    graph->AddOwnedEdge(right, binary, std::move(rightToBinary));
    graph->AddOwnedEdge(binary, sink, std::move(binaryToSink));
    graph->Build();

    std::string error;
    ASSERT_TRUE(graph->Validate(&error)) << error;
    TThreadedScheduler scheduler(*graph, 3);
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_EQ(binaryState->LeftRows, 3);
    EXPECT_EQ(binaryState->RightRows, 21);
    EXPECT_EQ(sinkState->Batches, 1);
    EXPECT_EQ(sinkState->Rows, 321);
    EXPECT_EQ(destroyCount->load(std::memory_order_relaxed), 5);
}

TEST(SchedulerRuntimeAdapter, HashShuffleTaskScattersSelectedRows) {
    std::atomic<int> destroyCount = 0;
    TOneToOneConnection input(1);
    THashShuffleConnection output(2);
    output.Resize(1, 2);

    std::vector<uint8_t> selection = {0xff, 0, 0xff, 0xff};
    auto rowSet = MakeRowSet(4, &destroyCount);
    rowSet.Selection = selection.data();
    ASSERT_TRUE(input.Push(0, std::move(rowSet)));
    input.Finish(0);

    auto code = std::make_shared<THashShuffleCode>(
        [](TRowSet* batch, uint64_t* hashes) {
            EXPECT_EQ(batch->RowCount, 4);
            hashes[0] = 0;
            hashes[1] = 1;
            hashes[2] = 1;
            hashes[3] = 0;
            return true;
        });
    auto state = std::make_shared<int>(0);
    THashShuffleTask task(
        code,
        state,
        TInputPort{.Connection = &input},
        output,
        0);

    EXPECT_EQ(task.Execute(), ETaskResult::OK);

    TRowSet left{};
    ASSERT_EQ(output.Fetch(0, left), EFetchResult::OK);
    ASSERT_NE(left.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(left.Selection, left.Selection + left.RowCount),
        (std::vector<uint8_t>{0xff, 0, 0, 0xff}));
    Release(&left);

    TRowSet right{};
    ASSERT_EQ(output.Fetch(1, right), EFetchResult::OK);
    ASSERT_NE(right.Selection, nullptr);
    EXPECT_EQ(
        std::vector<uint8_t>(right.Selection, right.Selection + right.RowCount),
        (std::vector<uint8_t>{0, 0, 0xff, 0}));
    Release(&right);

    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);
    EXPECT_EQ(output.Fetch(0, left), EFetchResult::FINISHED);
    EXPECT_EQ(output.Fetch(1, right), EFetchResult::FINISHED);
    EXPECT_EQ(destroyCount.load(std::memory_order_relaxed), 1);
}

TEST(SchedulerRuntimeAdapter, HashShuffleTaskBatchesRowsUntilFinish) {
    using namespace NQumir::NAst;

    std::atomic<int> destroyCount = 0;
    TOneToOneConnection input(2);
    THashShuffleConnection output(4);
    output.Resize(1, 2);

    ASSERT_TRUE(input.Push(0, MakeInt64RowSet({0, 1}, &destroyCount)));
    ASSERT_TRUE(input.Push(0, MakeInt64RowSet({2, 3}, &destroyCount)));
    input.Finish(0);

    auto schema = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"value", std::make_shared<TIntegerType>()},
        });
    auto code = std::make_shared<THashShuffleCode>(
        [](TRowSet* batch, uint64_t* hashes) {
            auto* values = reinterpret_cast<int64_t*>(batch->Columns[0].Data);
            for (int64_t row = 0; row < batch->RowCount; ++row) {
                hashes[row] = static_cast<uint64_t>(values[row]);
            }
            return true;
        },
        schema,
        16,
        64,
        1024 * 1024);
    auto state = std::make_shared<int>(0);
    THashShuffleTask task(
        code,
        state,
        TInputPort{.Connection = &input},
        output,
        0);

    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    TRowSet left{};
    EXPECT_EQ(output.Fetch(0, left), EFetchResult::NO_DATA);

    EXPECT_EQ(task.Execute(), ETaskResult::OK);
    EXPECT_EQ(output.Fetch(0, left), EFetchResult::NO_DATA);

    EXPECT_EQ(task.Execute(), ETaskResult::FINISHED);

    ASSERT_EQ(output.Fetch(0, left), EFetchResult::OK);
    ASSERT_EQ(left.RowCount, 2);
    ASSERT_EQ(left.Selection, nullptr);
    auto* leftValues = reinterpret_cast<int64_t*>(left.Columns[0].Data);
    EXPECT_EQ(std::vector<int64_t>(leftValues, leftValues + left.RowCount),
        (std::vector<int64_t>{0, 2}));
    Release(&left);

    TRowSet right{};
    ASSERT_EQ(output.Fetch(1, right), EFetchResult::OK);
    ASSERT_EQ(right.RowCount, 2);
    ASSERT_EQ(right.Selection, nullptr);
    auto* rightValues = reinterpret_cast<int64_t*>(right.Columns[0].Data);
    EXPECT_EQ(std::vector<int64_t>(rightValues, rightValues + right.RowCount),
        (std::vector<int64_t>{1, 3}));
    Release(&right);

    EXPECT_EQ(output.Fetch(0, left), EFetchResult::FINISHED);
    EXPECT_EQ(output.Fetch(1, right), EFetchResult::FINISHED);
    EXPECT_EQ(destroyCount.load(std::memory_order_relaxed), 2);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
