#include <gtest/gtest.h>

#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace NQqb;
using namespace NQumir::NAst;

TExprPtr ParsePredicate(const std::string& source) {
    std::istringstream input(source);
    NCore::TTokenStream tokens(input);
    NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    return std::move(*parsed);
}

TEST(FilterKernel, ComparesStringColumnsWithoutMutatingLogicalPredicate) {
    const std::string leftData = "absamez";
    const std::string rightData = "basamey";
    std::array<int32_t, 5> offsets = {0, 1, 2, 6, 7};
    std::array<TColumn, 2> columns = {
        TColumn{.Data = const_cast<char*>(leftData.data()), .Mask = nullptr,
            .Offsets = offsets.data(), .OffsetWidth = 4},
        TColumn{.Data = const_cast<char*>(rightData.data()), .Mask = nullptr,
            .Offsets = offsets.data(), .OffsetWidth = 4},
    };
    TStructType inputType({
        {"left", std::make_shared<TStringType>()},
        {"right", std::make_shared<TStringType>()},
    });
    const std::array<std::pair<const char*, std::array<uint8_t, 4>>, 6> cases = {{
        {"==", {0, 0, 0xff, 0}},
        {"!=", {0xff, 0xff, 0, 0xff}},
        {"<", {0xff, 0, 0, 0}},
        {"<=", {0xff, 0, 0xff, 0}},
        {">", {0, 0xff, 0, 0xff}},
        {">=", {0, 0xff, 0xff, 0xff}},
    }};

    for (const auto& [op, expected] : cases) {
        auto predicate = ParsePredicate(
            "(" + std::string(op) + " left right)");
        const std::string logical = NCore::PrintAst(predicate);
        auto dispatch = TKernelCompiler().CompileFilter(inputType, predicate);
        std::array<uint8_t, 4> selection{};
        TRowSet rowSet{
            .Columns = columns.data(),
            .ColumnCount = static_cast<int64_t>(columns.size()),
            .RowCount = static_cast<int64_t>(selection.size()),
            .Selection = selection.data(),
            .RefCount = 1,
        };
        dispatch(rowSet);
        EXPECT_EQ(selection, expected) << op;
        EXPECT_EQ(NCore::PrintAst(predicate), logical) << op;
    }
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
