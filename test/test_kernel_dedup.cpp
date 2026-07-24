#include <gtest/gtest.h>

#include <qdb/io/io.h>
#include <qdb/kernel/annotate_type.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/finalize.h>
#include <qdb/kernel/spec.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include <array>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace NQdb;
using namespace NQumir::NAst;

namespace {

TExprPtr Parse(const std::string& src) {
    std::istringstream in(src);
    NCore::TTokenStream tokens(in);
    NCore::TParser parser;
    auto parsed = parser.Parse(tokens);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }
    return *parsed;
}

} // namespace

TEST(KernelDedup, ReusesIdenticalProjectKernels) {
    TStructType inputType({
        {"value", std::make_shared<TIntegerType>(TIntegerType::I64)},
    });
    std::vector<TExprPtr> exprs = {Parse("(+ value 10)")};
    std::vector<TTypePtr> types = {
        NKernel::AnnotateExprType(exprs[0], inputType),
    };
    auto spec = NKernel::BuildProjectKernelSpec(inputType, exprs, types);

    std::vector<TGeneratedKernel> generated;
    TKernelCompiler compiler(TKernelCompilerOptions{
        .Sink = &generated,
        .BindNow = false,
    });
    auto dispatchA = compiler.CompileProject(spec);
    auto dispatchB = compiler.CompileProject(spec);

    ASSERT_EQ(generated.size(), 2u);
    ASSERT_FALSE(generated[0].Slot->Runner);
    ASSERT_FALSE(generated[1].Slot->Runner);

    JitFinalizeKernels(generated);

    ASSERT_TRUE(generated[0].Slot->Runner);
    ASSERT_TRUE(generated[1].Slot->Runner);
    ASSERT_EQ(generated[0].Slot->Runner, generated[1].Slot->Runner);
    ASSERT_EQ(generated[0].Slot->Fns.size(), 1u);
    ASSERT_EQ(generated[1].Slot->Fns.size(), 1u);
    ASSERT_EQ(generated[0].Slot->Fns[0], generated[1].Slot->Fns[0]);

    std::array<int64_t, 2> values = {1, 2};
    std::array<TColumn, 1> cols = {
        TColumn{.Data = reinterpret_cast<char*>(values.data())},
    };
    TRowSet batch{.Columns = cols.data(), .ColumnCount = 1, .RowCount = 2, .RefCount = 1};
    std::array<int64_t, 2> outputA{};
    std::array<int64_t, 2> outputB{};
    std::array<void*, 1> outBuffersA = {outputA.data()};
    std::array<void*, 1> outBuffersB = {outputB.data()};

    dispatchA(&batch, outBuffersA.data(), nullptr);
    dispatchB(&batch, outBuffersB.data(), nullptr);

    EXPECT_EQ(outputA, (std::array<int64_t, 2>{11, 12}));
    EXPECT_EQ(outputB, outputA);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
