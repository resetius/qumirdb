#include <gtest/gtest.h>

#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/graph.h>
#include <qdb/scheduler/single_threaded_scheduler.h>

#include <memory>
#include <string>

using namespace NQdb::NScheduler;

namespace {

class TNoopTask : public ITaskNode {
public:
    ETaskResult Execute() override {
        ++Calls_;
        return ETaskResult::FINISHED;
    }

    int Calls() const {
        return Calls_;
    }

private:
    int Calls_ = 0;
};

class TSourceTask : public ITaskNode {
public:
    ETaskResult Execute() override {
        if (!Produced) {
            Produced = true;
            return ETaskResult::OK;
        }
        Finished = true;
        return ETaskResult::FINISHED;
    }

    bool Produced = false;
    bool Finished = false;
};

class TSinkTask : public ITaskNode {
public:
    explicit TSinkTask(TSourceTask& source)
        : Source_(source)
    {}

    ETaskResult Execute() override {
        if (!Source_.Produced) {
            return ETaskResult::NEED_DATA;
        }
        if (!Consumed_) {
            Consumed_ = true;
            return ETaskResult::OK;
        }
        if (Source_.Finished) {
            Finished = true;
            return ETaskResult::FINISHED;
        }
        return ETaskResult::NEED_DATA;
    }

    bool Consumed() const {
        return Consumed_;
    }

    bool Finished = false;

private:
    TSourceTask& Source_;
    bool Consumed_ = false;
};

TEST(SchedulerGraph, BuildsAdjacencyAndValidates) {
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddEdge(source, sink, std::make_unique<TOneToOneConnection>());
    graph.Build();

    std::string error;
    EXPECT_TRUE(graph.Validate(&error)) << error;
    ASSERT_EQ(graph.Leaves().size(), 1u);
    EXPECT_EQ(graph.Leaves()[0], &source);
    EXPECT_EQ(graph.Root(), &sink);
    EXPECT_EQ(source.Outbound.size(), 1u);
    EXPECT_EQ(sink.Inbound.size(), 1u);
}

TEST(SchedulerGraph, RejectsMissingConnection) {
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddEdge(source, sink, {});
    graph.Build();

    std::string error;
    EXPECT_FALSE(graph.Validate(&error));
    EXPECT_NE(error.find("without a connection"), std::string::npos);
}

TEST(SchedulerGraph, RejectsMultipleRoots) {
    TTaskGraph graph;
    graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.Build();

    std::string error;
    EXPECT_FALSE(graph.Validate(&error));
    EXPECT_NE(error.find("exactly one root"), std::string::npos);
}

TEST(SchedulerGraph, RejectsInvalidLaneIds) {
    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    auto& sink = graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.AddEdge(source, sink, std::make_unique<TOneToOneConnection>(), 1, 0);
    graph.Build();

    std::string error;
    EXPECT_FALSE(graph.Validate(&error));
    EXPECT_NE(error.find("source lane"), std::string::npos);
}

TEST(SchedulerGraph, SingleThreadedSchedulerRunsSmallGraph) {
    auto sourceTask = std::make_unique<TSourceTask>();
    auto* sourcePtr = sourceTask.get();
    auto sinkTask = std::make_unique<TSinkTask>(*sourcePtr);
    auto* sinkPtr = sinkTask.get();

    TTaskGraph graph;
    auto& source = graph.AddOwnedNode(std::move(sourceTask));
    auto& sink = graph.AddOwnedNode(std::move(sinkTask));
    graph.AddEdge(source, sink, std::make_unique<TOneToOneConnection>());
    graph.Build();

    TSingleThreadedScheduler scheduler(graph);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_TRUE(sourcePtr->Finished);
    EXPECT_TRUE(sinkPtr->Consumed());
    EXPECT_TRUE(sinkPtr->Finished);
}

TEST(SchedulerGraph, SingleThreadedSchedulerCannotRunTwice) {
    TTaskGraph graph;
    graph.AddOwnedNode(std::make_unique<TNoopTask>());
    graph.Build();

    TSingleThreadedScheduler scheduler(graph);
    std::string error;
    ASSERT_TRUE(scheduler.Run(&error)) << error;
    EXPECT_FALSE(scheduler.Run(&error));
    EXPECT_NE(error.find("cannot run twice"), std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
