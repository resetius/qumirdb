#include <gtest/gtest.h>

#include <string>

namespace {

using TTableValues = std::map<std::string, std::string>;

struct TTableRow {
    int Key;
    TTableValues Values;
};

using TTable = std::vector<TTableRow>;

int RandomInt(int* seed) {
    *seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
    return *seed;
}

std::string RandomString(int* seed, size_t length) {
    const std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += chars[RandomInt(seed) % chars.size()];
    }
    return result;
}

std::vector<TTableRow> GenerateTable(int* seed, int numKeys, int numRows, std::set<std::string> columns) {
    std::vector<TTableRow> table;
    table.reserve(numRows);
    for (int i = 0; i < numRows; ++i) {
        auto key = RandomInt(seed) % numKeys;
        TTableValues values;
        for (const auto& col : columns) {
            values[col] = RandomString(seed, 10);
        }
        table.push_back({key, values});
    }
    return table;
}

TTable InnerJoin(const TTable& left, const TTable& right) {
    int batchSize = 10;
    // key -> rows
    using THashMap = std::unordered_map<int, std::vector<TTableValues>>;
    THashMap leftHash;
    THashMap rightHash;
    int leftIt = 0;
    int rightIt = 0;

    TTable result;

    auto progress = [&](auto& leftIt, const TTable& left, const TTable& right, THashMap& leftHash, THashMap& rightHash) -> bool {
        bool hasMore = leftIt < left.size();
        if (!hasMore) {
            return false;
        }
        for (int i = 0; i < batchSize && leftIt < left.size(); ++i, ++leftIt) {
            auto& tableRow = left[leftIt];
            auto& key = tableRow.Key;
            auto& row = tableRow.Values;
            // 1. try join with right rows for this key
            auto rightIt = rightHash.find(key);
            if (rightIt != rightHash.end()) {
                for (const auto& rightRow : rightIt->second) {
                    TTableRow joinedRow;
                    joinedRow.Key = key;
                    joinedRow.Values.insert(row.begin(), row.end());
                    joinedRow.Values.insert(rightRow.begin(), rightRow.end());
                    result.push_back(joinedRow);
                }
            }
            // 2. store left row for future join
            leftHash[key].push_back(row);
        }
        return true;
    };

    while (progress(leftIt, left, right, leftHash, rightHash) || progress(rightIt, right, left, rightHash, leftHash)) {
        // keep joining until both sides are exhausted
    }

    return result;
}

TTable NestedLoopInnerJoin(const TTable& left, const TTable& right) {
    TTable result;
    for (const auto& leftRow : left) {
        for (const auto& rightRow : right) {
            if (leftRow.Key == rightRow.Key) {
                TTableRow joinedRow;
                joinedRow.Key = leftRow.Key;
                joinedRow.Values.insert(leftRow.Values.begin(), leftRow.Values.end());
                joinedRow.Values.insert(rightRow.Values.begin(), rightRow.Values.end());
                result.push_back(joinedRow);
            }
        }
    }
    return result;
}

void CheckJoinMatchesNestedLoop(const TTable& left, const TTable& right) {
    auto joinedTable = InnerJoin(left, right);
    auto nestedLoopJoinedTable = NestedLoopInnerJoin(left, right);
    EXPECT_EQ(joinedTable.size(), nestedLoopJoinedTable.size());
    for (const auto& row : joinedTable) {
        auto it = std::find_if(nestedLoopJoinedTable.begin(), nestedLoopJoinedTable.end(), [&](const TTableRow& r) {
            return r.Key == row.Key && r.Values == row.Values;
        });
        EXPECT_NE(it, nestedLoopJoinedTable.end());
    }
}

void PrintTable(const TTable& table, std::ostream& os) {
    for (const auto& row : table) {
        os << "Key: " << row.Key << ", Values: ";
        for (const auto& [col, val] : row.Values) {
            os << col << "=" << val << " ";
        }
        os << std::endl;
    }
}

} // namespace

TEST(JoinTest, BasicInnerJoin) {
    int seed = 42;
    const int numLeftKeys = 5;
    const int numRightKeys = 5;
    const int numLeftRows = 5;
    const int numRightRows = 6;
    std::set<std::string> leftColumns = {"col1", "col2"};
    std::set<std::string> rightColumns = {"col3", "col4"};
    auto leftTable = GenerateTable(&seed, numLeftKeys, numLeftRows, leftColumns);
    auto rightTable = GenerateTable(&seed, numRightKeys, numRightRows, rightColumns);
    auto joinedTable = InnerJoin(leftTable, rightTable);

    std::cout << "Left Table:" << std::endl;
    PrintTable(leftTable, std::cout);
    std::cout << "Right Table:" << std::endl;
    PrintTable(rightTable, std::cout);
    std::cout << "Joined Table:" << std::endl;
    PrintTable(joinedTable, std::cout);

    auto nestedLoopJoinedTable = NestedLoopInnerJoin(leftTable, rightTable);
    EXPECT_EQ(joinedTable.size(), nestedLoopJoinedTable.size());
    for (const auto& row : joinedTable) {
        auto it = std::find_if(nestedLoopJoinedTable.begin(), nestedLoopJoinedTable.end(), [&](const TTableRow& r) {
            return r.Key == row.Key && r.Values == row.Values;
        });
        EXPECT_NE(it, nestedLoopJoinedTable.end());
    }
}

TEST(JoinTest, LargeTablesManyKeys) {
    int seed = 123;
    const int numLeftKeys = 20;
    const int numRightKeys = 20;
    const int numLeftRows = 37;
    const int numRightRows = 42;
    std::set<std::string> leftColumns = {"col1", "col2"};
    std::set<std::string> rightColumns = {"col3", "col4"};
    auto leftTable = GenerateTable(&seed, numLeftKeys, numLeftRows, leftColumns);
    auto rightTable = GenerateTable(&seed, numRightKeys, numRightRows, rightColumns);

    CheckJoinMatchesNestedLoop(leftTable, rightTable);
}

TEST(JoinTest, FewKeysHighCardinality) {
    int seed = 7;
    const int numLeftKeys = 2;
    const int numRightKeys = 2;
    const int numLeftRows = 15;
    const int numRightRows = 18;
    std::set<std::string> leftColumns = {"col1"};
    std::set<std::string> rightColumns = {"col2"};
    auto leftTable = GenerateTable(&seed, numLeftKeys, numLeftRows, leftColumns);
    auto rightTable = GenerateTable(&seed, numRightKeys, numRightRows, rightColumns);

    CheckJoinMatchesNestedLoop(leftTable, rightTable);
}

TEST(JoinTest, SingleKeyFullCrossProduct) {
    int seed = 99;
    const int numLeftKeys = 1;
    const int numRightKeys = 1;
    const int numLeftRows = 8;
    const int numRightRows = 11;
    std::set<std::string> leftColumns = {"col1"};
    std::set<std::string> rightColumns = {"col2"};
    auto leftTable = GenerateTable(&seed, numLeftKeys, numLeftRows, leftColumns);
    auto rightTable = GenerateTable(&seed, numRightKeys, numRightRows, rightColumns);

    CheckJoinMatchesNestedLoop(leftTable, rightTable);
}

TEST(JoinTest, AsymmetricSidesAndKeyRanges) {
    int seed = 555;
    const int numLeftKeys = 3;
    const int numRightKeys = 15;
    const int numLeftRows = 5;
    const int numRightRows = 50;
    std::set<std::string> leftColumns = {"col1"};
    std::set<std::string> rightColumns = {"col2", "col3"};
    auto leftTable = GenerateTable(&seed, numLeftKeys, numLeftRows, leftColumns);
    auto rightTable = GenerateTable(&seed, numRightKeys, numRightRows, rightColumns);

    CheckJoinMatchesNestedLoop(leftTable, rightTable);
}

TEST(JoinTest, EmptySides) {
    int seed = 1;
    std::set<std::string> leftColumns = {"col1"};
    std::set<std::string> rightColumns = {"col2"};

    auto leftTable = GenerateTable(&seed, 5, 10, leftColumns);
    auto emptyRight = GenerateTable(&seed, 5, 0, rightColumns);
    CheckJoinMatchesNestedLoop(leftTable, emptyRight);

    auto emptyLeft = GenerateTable(&seed, 5, 0, leftColumns);
    auto rightTable = GenerateTable(&seed, 5, 10, rightColumns);
    CheckJoinMatchesNestedLoop(emptyLeft, rightTable);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
