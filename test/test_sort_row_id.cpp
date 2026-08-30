#include <gtest/gtest.h>

#include <qdb/exec/join_exec.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/lib.h>
#include <qdb/plan/types/nullable.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* TestSortRowIdWrapperSource = R"(
(block
  (fun test_sort_row_ids
       ((var rows <ptr i64>)
        (var size i64)
        (var store <ptr TRowSet>))
    (block
      (call heap_sort rows size store))))
)";

struct TCompiledRowIdSort {
    using TSort = void (*)(int64_t*, int64_t, NQdb::TRowSet*);

    std::unique_ptr<NQumir::TLLVMRunner> Runner;
    TSort Sort = nullptr;
};

TCompiledRowIdSort
CompileRowIdSort(const std::vector<NQdb::TSortRadixKeyInput>& keys) {
    std::vector<NQumir::NAst::TExprPtr> statements;
    auto heap = NQdb::NKernel::ParseFunctionLibrary(
        NQdb::NKernel::ReadSortKernel("heap.oz"));
    if (!heap) {
        ADD_FAILURE() << heap.error().ToString();
        return {};
    }
    for (auto& statement : *heap) {
        statements.push_back(std::move(statement));
    }
    statements.push_back(NQdb::BuildSortRowIdLessAst(keys));

    auto wrapper =
        NQdb::NKernel::ParseFunctionLibrary(TestSortRowIdWrapperSource);
    if (!wrapper) {
        ADD_FAILURE() << wrapper.error().ToString();
        return {};
    }
    for (auto& statement : *wrapper) {
        statements.push_back(std::move(statement));
    }

    auto options = NQdb::KernelRunnerOptions();
    options.NativeCode = true;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    auto program = std::make_shared<NQumir::NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(statements));
    std::string error;
    auto entries = NQdb::CompileKernelAst(*runner, std::move(program),
                                          {"test_sort_row_ids"}, &error);
    auto entry = entries.find("test_sort_row_ids");
    if (entry == entries.end() || !entry->second) {
        ADD_FAILURE() << error;
        return {.Runner = std::move(runner)};
    }
    return {
        .Runner = std::move(runner),
        .Sort = reinterpret_cast<TCompiledRowIdSort::TSort>(entry->second),
    };
}

std::vector<int64_t> MakeRowIds(std::array<NQdb::TRowSet, 2>& store) {
    std::vector<int64_t> rows;
    for (int32_t batch = 0; batch < static_cast<int32_t>(store.size());
         ++batch) {
        for (int32_t row = 0; row < store[batch].RowCount; ++row) {
            rows.push_back(NQdb::MakeRowId(batch, row));
        }
    }
    return rows;
}

TEST(SortRowId, SortsSeveralBatchesByMixedNumericKeys) {
    using namespace NQumir::NAst;

    auto i64 = std::make_shared<TIntegerType>();
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto compiled = CompileRowIdSort({
        {.ColumnIndex = 0, .Type = i64, .Desc = true},
        {.ColumnIndex = 1, .Type = i32},
    });
    ASSERT_NE(compiled.Sort, nullptr);

    std::array<int64_t, 3> primary0 = {2, 1, 2};
    std::array<int32_t, 3> secondary0 = {1, 9, 1};
    std::array<int64_t, 3> primary1 = {2, 1, 2};
    std::array<int32_t, 3> secondary1 = {0, 9, 1};
    std::array<NQdb::TColumn, 2> columns0 = {
        NQdb::TColumn{.Data = reinterpret_cast<char*>(primary0.data())},
        NQdb::TColumn{.Data = reinterpret_cast<char*>(secondary0.data())},
    };
    std::array<NQdb::TColumn, 2> columns1 = {
        NQdb::TColumn{.Data = reinterpret_cast<char*>(primary1.data())},
        NQdb::TColumn{.Data = reinterpret_cast<char*>(secondary1.data())},
    };
    std::array<NQdb::TRowSet, 2> store = {
        NQdb::TRowSet{
            .Columns = columns0.data(),
            .ColumnCount = static_cast<int64_t>(columns0.size()),
            .RowCount = static_cast<int64_t>(primary0.size()),
        },
        NQdb::TRowSet{
            .Columns = columns1.data(),
            .ColumnCount = static_cast<int64_t>(columns1.size()),
            .RowCount = static_cast<int64_t>(primary1.size()),
        },
    };

    auto rows = MakeRowIds(store);
    compiled.Sort(rows.data(), static_cast<int64_t>(rows.size()),
                  store.data());

    EXPECT_EQ(rows, (std::vector<int64_t>{
                                NQdb::MakeRowId(1, 0),
                                NQdb::MakeRowId(0, 0),
                                NQdb::MakeRowId(0, 2),
                                NQdb::MakeRowId(1, 2),
                                NQdb::MakeRowId(0, 1),
                                NQdb::MakeRowId(1, 1),
                            }));
}

struct TStringColumn {
    std::string Data;
    std::vector<int32_t> Offsets;
    std::vector<uint8_t> Mask;
    NQdb::TColumn Column{};

    TStringColumn(const std::vector<std::string>& values,
                  std::vector<uint8_t> mask)
        : Offsets(values.size() + 1, 0), Mask(std::move(mask)) {
        for (size_t i = 0; i < values.size(); ++i) {
            Data += values[i];
            Offsets[i + 1] = static_cast<int32_t>(Data.size());
        }
        Column = {
            .Data = Data.data(),
            .Mask = Mask.data(),
            .Offsets = Offsets.data(),
            .OffsetWidth = 4,
        };
    }
};

std::vector<int64_t> MakeSingleBatchRowIds(const NQdb::TRowSet& batch) {
    std::vector<int64_t> rows;
    for (int32_t row = 0; row < batch.RowCount; ++row) {
        rows.push_back(NQdb::MakeRowId(0, row));
    }
    return rows;
}

TEST(SortRowId, SortsNullableStringsWithSqlNullOrder) {
    using namespace NQumir::NAst;

    TStringColumn strings({"12345678beta", "unused", "12345678alpha",
                           "12345678beta", "unused", "12345678alpha"},
                          {0b00101101});
    NQdb::TRowSet batch = {
        .Columns = &strings.Column,
        .ColumnCount = 1,
        .RowCount = 6,
    };
    auto stringType =
        std::make_shared<NQdb::TNullable>(std::make_shared<TStringType>());

    auto nullsFirst = CompileRowIdSort({{
        .ColumnIndex = 0,
        .Type = stringType,
        .NullsFirst = true,
    }});
    ASSERT_NE(nullsFirst.Sort, nullptr);
    auto rows = MakeSingleBatchRowIds(batch);
    nullsFirst.Sort(rows.data(), static_cast<int64_t>(rows.size()), &batch);
    EXPECT_EQ(rows, (std::vector<int64_t>{
                                NQdb::MakeRowId(0, 1),
                                NQdb::MakeRowId(0, 4),
                                NQdb::MakeRowId(0, 2),
                                NQdb::MakeRowId(0, 5),
                                NQdb::MakeRowId(0, 0),
                                NQdb::MakeRowId(0, 3),
                            }));

    auto descNullsLast = CompileRowIdSort({{
        .ColumnIndex = 0,
        .Type = stringType,
        .Desc = true,
        .NullsFirst = false,
    }});
    ASSERT_NE(descNullsLast.Sort, nullptr);
    rows = MakeSingleBatchRowIds(batch);
    descNullsLast.Sort(rows.data(), static_cast<int64_t>(rows.size()), &batch);
    EXPECT_EQ(rows, (std::vector<int64_t>{
                                NQdb::MakeRowId(0, 0),
                                NQdb::MakeRowId(0, 3),
                                NQdb::MakeRowId(0, 2),
                                NQdb::MakeRowId(0, 5),
                                NQdb::MakeRowId(0, 1),
                                NQdb::MakeRowId(0, 4),
                            }));
}

} // namespace

int main(int argc, char** argv) {
    NQumir::NCodeGen::TLLVMInitializer initializer;
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
