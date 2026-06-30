#include <gtest/gtest.h>

#include <qdb/scheduler/settings.h>
#include <qdb/scheduler/state.h>

using namespace NQdb::NScheduler;

namespace {

class TTestTask : public ITaskNode {
public:
    ETaskResult Execute() override {
        ++Calls_;
        return ETaskResult::OK;
    }

    int Calls() const {
        return Calls_;
    }

private:
    int Calls_ = 0;
};

TEST(SchedulerCore, TaskNodeInterfaceExecutes) {
    TTestTask task;
    ITaskNode* node = &task;

    EXPECT_EQ(node->Execute(), ETaskResult::OK);
    EXPECT_EQ(task.Calls(), 1);
}

TEST(SchedulerCore, TaskStateValuesAreStable) {
    EXPECT_EQ(static_cast<int>(ETaskState::Idle), 0);
    EXPECT_EQ(static_cast<int>(ETaskState::Queued), 1);
    EXPECT_EQ(static_cast<int>(ETaskState::Running), 2);
    EXPECT_EQ(static_cast<int>(ETaskState::Reschedule), 3);
    EXPECT_EQ(static_cast<int>(ETaskState::Finished), 4);
}

TEST(SchedulerCore, SettingsDefaultsAreConservative) {
    TSettings settings;

    EXPECT_EQ(settings.Scheduler.Mode, EExecutionMode::Serial);
    EXPECT_EQ(settings.Scheduler.WorkerCount, 1u);
    EXPECT_EQ(settings.Partitioning.DefaultPartitionCount, 1u);
    EXPECT_EQ(settings.Partitioning.MaxPartitionCount, 1u);
    EXPECT_EQ(settings.Queue.RowsetCapacityPerLane, 1u);
    EXPECT_EQ(settings.ScanSplit.MaxScanTasks, 1u);
    EXPECT_EQ(settings.ScanSplit.RowGroupCoalescingFactor, 1u);
    EXPECT_EQ(settings.HashShuffle.PartitionCount, 1u);
    EXPECT_EQ(settings.HashShuffle.MaxPartitionCount, 1u);
    EXPECT_EQ(settings.Sort.MergeFanIn, 2u);
    EXPECT_TRUE(settings.KernelHelper.EnableRowsetHashKernel);
    EXPECT_TRUE(settings.KernelHelper.EnableCompareKernel);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
