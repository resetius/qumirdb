#include <gtest/gtest.h>

#include <qdb/kernel/finalize_fused.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace NQumir;
using namespace NQumir::NAst;

TTypePtr I64() {
    return std::make_shared<TIntegerType>(TIntegerType::I64);
}

TTypePtr F64() {
    return std::make_shared<TFloatType>();
}

TTypePtr Void() {
    return std::make_shared<TVoidType>();
}

TTypePtr Struct(std::vector<std::pair<std::string, TTypePtr>> fields) {
    return std::make_shared<TStructType>(std::move(fields));
}

TTypePtr Named(std::string name, TTypePtr underlying = {}) {
    return std::make_shared<TNamedType>(std::move(name), std::move(underlying));
}

TExprPtr TypeDecl(std::string name, TTypePtr underlying) {
    return std::make_shared<TTypeDeclStmt>(
        TLocation{}, Named(std::move(name), std::move(underlying)));
}

TExprPtr Function(std::string name, std::vector<TTypePtr> paramTypes = {}) {
    std::vector<TParam> params;
    params.reserve(paramTypes.size());
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        params.push_back(std::make_shared<TVarStmt>(
            TLocation{}, "arg" + std::to_string(i), paramTypes[i]));
    }
    auto ret = Void();
    auto fun = std::make_shared<TFunDecl>(
        TLocation{}, std::move(name), std::vector<TGenericParam>{},
        std::move(params), std::make_shared<TBlockExpr>(
            TLocation{}, std::vector<TExprPtr>{}),
        ret);
    fun->Type = std::make_shared<TFunctionType>(std::move(paramTypes), ret);
    return fun;
}

NQdb::TGeneratedKernel Kernel(
    std::string name,
    std::vector<TExprPtr> stmts,
    std::vector<std::string> entrypoints)
{
    return NQdb::TGeneratedKernel{
        .Name = std::move(name),
        .Stage = "stage",
        .Entrypoints = std::move(entrypoints),
        .Ast = std::make_shared<TBlockExpr>(TLocation{}, std::move(stmts)),
    };
}

std::vector<std::string> TypeDeclNames(const TExprPtr& root) {
    auto block = TMaybeNode<TBlockExpr>(root);
    EXPECT_TRUE(block);
    std::vector<std::string> names;
    for (const auto& stmt : block.Cast()->Stmts) {
        auto typeDecl = TMaybeNode<TTypeDeclStmt>(stmt);
        if (!typeDecl) {
            continue;
        }
        auto named = TMaybeType<TNamedType>(typeDecl.Cast()->Type);
        if (!named) {
            ADD_FAILURE() << "type declaration is not a named type";
            return names;
        }
        names.push_back(named.Cast()->Name);
    }
    return names;
}

std::vector<std::string> FunctionDeclNames(const TExprPtr& root) {
    auto block = TMaybeNode<TBlockExpr>(root);
    EXPECT_TRUE(block);
    std::vector<std::string> names;
    for (const auto& stmt : block.Cast()->Stmts) {
        auto fun = TMaybeNode<TFunDecl>(stmt);
        if (fun) {
            names.push_back(fun.Cast()->Name);
        }
    }
    return names;
}

} // namespace

TEST(FinalizeFused, RenamesTypesThatDependOnRenamedLocalTypes) {
    auto k0 = Kernel("k0", {
        TypeDecl("A", Struct({{"x", I64()}})),
        TypeDecl("B", Struct({{"a", Named("A")}})),
        Function("entry0"),
    }, {"entry0"});
    auto k1 = Kernel("k1", {
        TypeDecl("A", Struct({{"x", F64()}})),
        TypeDecl("B", Struct({{"a", Named("A")}})),
        Function("entry1"),
    }, {"entry1"});

    std::array<NQdb::TGeneratedKernel*, 2> kernels{&k0, &k1};
    auto fused = NQdb::BuildFusedProgram(
        std::span<NQdb::TGeneratedKernel* const>(kernels.data(), kernels.size()));

    EXPECT_EQ(fused.TypeDeclCount, 4u);
    EXPECT_EQ(TypeDeclNames(fused.Program), (std::vector<std::string>{
        "A", "B", "__qdb_k1_A", "__qdb_k1_B"}));
}

TEST(FinalizeFused, RenamesDuplicateEntrypointNamesEvenWithDifferentSignatures) {
    auto k0 = Kernel("k0", {Function("filter", {I64()})}, {"filter"});
    auto k1 = Kernel("k1", {Function("filter", {F64()})}, {"filter"});

    std::array<NQdb::TGeneratedKernel*, 2> kernels{&k0, &k1};
    auto fused = NQdb::BuildFusedProgram(
        std::span<NQdb::TGeneratedKernel* const>(kernels.data(), kernels.size()));

    ASSERT_EQ(fused.UniqueEntrypoints.size(), 2u);
    EXPECT_EQ(fused.UniqueEntrypoints[0], (std::vector<std::string>{"filter"}));
    EXPECT_EQ(fused.UniqueEntrypoints[1], (std::vector<std::string>{"__qdb_k1_filter"}));
    EXPECT_EQ(fused.Entrypoints, (std::vector<std::string>{"filter", "__qdb_k1_filter"}));
    EXPECT_EQ(FunctionDeclNames(fused.Program), (std::vector<std::string>{
        "filter", "__qdb_k1_filter"}));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
