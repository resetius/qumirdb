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
#include <utility>
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

TGeneratedKernel MakeCoreKernel(
    std::string name,
    std::string entrypoint,
    std::string source)
{
    TGeneratedKernel kernel{
        .Name = std::move(name),
        .Entrypoints = {std::move(entrypoint)},
        .Ast = Parse(source),
        .Slot = std::make_shared<TKernelSlot>(),
    };
    kernel.Slot->Fns.resize(kernel.Entrypoints.size(), nullptr);
    return kernel;
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

TEST(KernelDedup, FusesDifferentProjectKernelsIntoOneRunner) {
    TStructType inputType({
        {"value", std::make_shared<TIntegerType>(TIntegerType::I64)},
    });

    auto exprA = Parse("(+ value 10)");
    auto exprB = Parse("(* value 10)");
    auto specA = NKernel::BuildProjectKernelSpec(
        inputType,
        {exprA},
        {NKernel::AnnotateExprType(exprA, inputType)});
    auto specB = NKernel::BuildProjectKernelSpec(
        inputType,
        {exprB},
        {NKernel::AnnotateExprType(exprB, inputType)});

    std::vector<TGeneratedKernel> generated;
    TKernelCompiler compiler(TKernelCompilerOptions{
        .Sink = &generated,
        .BindNow = false,
    });
    auto dispatchA = compiler.CompileProject(specA);
    auto dispatchB = compiler.CompileProject(specB);

    ASSERT_EQ(generated.size(), 2u);
    JitFinalizeKernels(generated);

    ASSERT_TRUE(generated[0].Slot->Runner);
    ASSERT_TRUE(generated[1].Slot->Runner);
    ASSERT_EQ(generated[0].Slot->Runner, generated[1].Slot->Runner);
    ASSERT_EQ(generated[0].Slot->Fns.size(), 1u);
    ASSERT_EQ(generated[1].Slot->Fns.size(), 1u);
    ASSERT_NE(generated[0].Slot->Fns[0], generated[1].Slot->Fns[0]);

    std::array<int64_t, 2> values = {2, 3};
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

    EXPECT_EQ(outputA, (std::array<int64_t, 2>{12, 13}));
    EXPECT_EQ(outputB, (std::array<int64_t, 2>{20, 30}));
}

TEST(KernelDedup, KeepsHelpersBoundToRenamedKernelFunctions) {
    std::vector<TGeneratedKernel> generated;
    generated.push_back(MakeCoreKernel(
        "first",
        "run",
        R"((block
  (fun calc ((var x i64)) -> i64 (block (return (+ x (: 1 i64)))))
  (fun helper ((var x i64)) -> i64 (block (return (call calc x))))
  (fun run ((var x i64)) -> i64 (block (return (call helper x))))))"));
    generated.push_back(MakeCoreKernel(
        "second",
        "run",
        R"((block
  (fun calc ((var x i64)) -> i64 (block (return (+ x (: 2 i64)))))
  (fun helper ((var x i64)) -> i64 (block (return (call calc x))))
  (fun run ((var x i64)) -> i64 (block (return (call helper x))))))"));

    JitFinalizeKernels(generated);

    ASSERT_TRUE(generated[0].Slot->Runner);
    ASSERT_TRUE(generated[1].Slot->Runner);
    ASSERT_EQ(generated[0].Slot->Runner, generated[1].Slot->Runner);
    ASSERT_EQ(generated[0].Slot->Fns.size(), 1u);
    ASSERT_EQ(generated[1].Slot->Fns.size(), 1u);
    ASSERT_NE(generated[0].Slot->Fns[0], generated[1].Slot->Fns[0]);

    using TFn = int64_t(*)(int64_t);
    auto first = reinterpret_cast<TFn>(generated[0].Slot->Fns[0]);
    auto second = reinterpret_cast<TFn>(generated[1].Slot->Fns[0]);

    EXPECT_EQ(first(10), 11);
    EXPECT_EQ(second(10), 12);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
