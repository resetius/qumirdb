#include <gtest/gtest.h>

#include <qdb/scheduler/scan_split.h>

#include <vector>

using namespace NQdb::NScheduler;

namespace {

std::vector<TScanRowGroup> RowGroups()
{
    return {
        {.RowGroup = 0, .RowCount = 10, .ByteSize = 100},
        {.RowGroup = 1, .RowCount = 20, .ByteSize = 200},
        {.RowGroup = 2, .RowCount = 30, .ByteSize = 300},
        {.RowGroup = 3, .RowCount = 40, .ByteSize = 400},
    };
}

TEST(SchedulerScanSplit, DefaultsToSingleSerialSplit) {
    TScanSplitSettings settings;
    auto splits = BuildScanSplits(RowGroups(), settings);

    ASSERT_EQ(splits.size(), 1u);
    EXPECT_EQ(splits[0].FirstRowGroup, 0u);
    EXPECT_EQ(splits[0].RowGroupCount, 4u);
    EXPECT_EQ(splits[0].RowCount, 100);
    EXPECT_EQ(splits[0].ByteSize, 1000);
    EXPECT_TRUE(splits[0].SerialRead);
}

TEST(SchedulerScanSplit, SplitsOnePerRowGroupWhenTasksAllow) {
    // 4 row groups, 4 tasks -> one row group per split.
    TScanSplitSettings settings;
    settings.Strategy = EScanSplitStrategy::RowGroupRange;
    settings.MaxScanTasks = 4;

    auto splits = BuildScanSplits(RowGroups(), settings);

    ASSERT_EQ(splits.size(), 4u);
    for (size_t i = 0; i < splits.size(); ++i) {
        EXPECT_EQ(splits[i].FirstRowGroup, i);
        EXPECT_EQ(splits[i].RowGroupCount, 1u);
        EXPECT_FALSE(splits[i].SerialRead);
    }
}

TEST(SchedulerScanSplit, SplitsEvenlyAndCapsAtMaxTasks) {
    // 4 row groups, 2 tasks -> two even contiguous splits of 2.
    TScanSplitSettings settings;
    settings.Strategy = EScanSplitStrategy::RowGroupRange;
    settings.MaxScanTasks = 2;

    auto splits = BuildScanSplits(RowGroups(), settings);

    ASSERT_EQ(splits.size(), 2u);
    EXPECT_EQ(splits[0].FirstRowGroup, 0u);
    EXPECT_EQ(splits[0].RowGroupCount, 2u);
    EXPECT_EQ(splits[0].RowCount, 30);
    EXPECT_EQ(splits[1].FirstRowGroup, 2u);
    EXPECT_EQ(splits[1].RowGroupCount, 2u);
    EXPECT_EQ(splits[1].RowCount, 70);
}

TEST(SchedulerScanSplit, KeepsTinyInputsSerial) {
    TScanSplitSettings settings;
    settings.Strategy = EScanSplitStrategy::Auto;
    settings.MaxScanTasks = 8;
    settings.TinyInputRowsThreshold = 100;

    auto splits = BuildScanSplits(RowGroups(), settings);

    ASSERT_EQ(splits.size(), 1u);
    EXPECT_TRUE(splits[0].SerialRead);
}

TEST(SchedulerScanSplit, SplitsContiguousRangesEvenly) {
    TScanSplitSettings settings;
    settings.Strategy = EScanSplitStrategy::RowGroupRange;
    settings.MaxScanTasks = 2;

    auto rowGroups = RowGroups();
    rowGroups.push_back({.RowGroup = 4, .RowCount = 50, .ByteSize = 500});

    auto splits = BuildScanSplits(rowGroups, settings);

    ASSERT_EQ(splits.size(), 2u);
    EXPECT_EQ(splits[0].FirstRowGroup, 0u);
    EXPECT_EQ(splits[0].RowGroupCount, 3u);
    EXPECT_EQ(splits[0].RowCount, 60);
    EXPECT_EQ(splits[1].FirstRowGroup, 3u);
    EXPECT_EQ(splits[1].RowGroupCount, 2u);
    EXPECT_EQ(splits[1].RowCount, 90);
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
