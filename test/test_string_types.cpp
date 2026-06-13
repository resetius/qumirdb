#include <gtest/gtest.h>

#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_types.h>
#include <qdb/io/io.h>
#include <qdb/kernel/column_value.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>

#include <cstdint>
#include <array>
#include <memory>
#include <string>

namespace {

using namespace NQumir;
using namespace NQumir::NAst;

TTypePtr FindExternalType(
    const NQumir::NRegistry::QumirDbModule& module,
    const std::string& name)
{
    for (const auto& type : module.ExternalTypes()) {
        if (type.Name == name) {
            return type.Type;
        }
    }
    return nullptr;
}

struct TModuleTypes {
    TTypePtr Column;
    TTypePtr StringView;
};

TModuleTypes GetModuleTypes(const NQumir::NRegistry::QumirDbModule& module) {
    return {
        .Column = FindExternalType(module, "TColumn"),
        .StringView = FindExternalType(module, "StringView"),
    };
}

std::unique_ptr<TLLVMRunner> CompileStringColumnReader(void*& entry) {
    NQumir::NRegistry::QumirDbModule module;
    auto types = GetModuleTypes(module);
    auto materialized = NQqb::NKernel::BuildColumnValueAst(
        "column", "row", "value", std::make_shared<TStringType>(),
        types.StringView);

    TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>();
    auto boolType = std::make_shared<TBoolType>();
    auto stringViewType = std::make_shared<TNamedType>(
        "StringView", types.StringView);
    auto columnType = std::make_shared<TNamedType>("TColumn", types.Column);
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "column",
            std::make_shared<TReferenceType>(columnType)),
        std::make_shared<TVarStmt>(loc, "row", i64Type),
        std::make_shared<TVarStmt>(loc, "output",
            std::make_shared<TPointerType>(stringViewType)),
        std::make_shared<TVarStmt>(loc, "valid",
            std::make_shared<TPointerType>(boolType)),
    };
    std::vector<TExprPtr> body = std::move(materialized.Setup);
    body.insert(body.end(), {
        std::make_shared<TArrayAssignExpr>(loc, "output",
            std::vector<TExprPtr>{ident("row")}, materialized.Value),
        std::make_shared<TArrayAssignExpr>(loc, "valid",
            std::vector<TExprPtr>{ident("row")}, materialized.IsValid),
        std::make_shared<TReturnExpr>(loc,
            std::make_shared<TFieldAccessExpr>(loc,
                std::make_shared<TIndexExpr>(loc, ident("output"), ident("row")),
                "Size")),
    });
    auto function = std::make_shared<TFunDecl>(loc, "read_string_column",
        std::move(params), std::make_shared<TBlockExpr>(loc, std::move(body)),
        i64Type);
    auto program = std::make_shared<TBlockExpr>(loc,
        std::vector<TExprPtr>{std::move(function)});

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    auto runner = std::make_unique<TLLVMRunner>(options);
    runner->RegisterModule(
        std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    std::string error;
    entry = runner->CompileKernelAst(
        std::move(program), "read_string_column", &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

std::unique_ptr<TLLVMRunner> CompileI32ColumnReader(void*& entry) {
    NQumir::NRegistry::QumirDbModule module;
    auto types = GetModuleTypes(module);
    auto i32Type = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto materialized = NQqb::NKernel::BuildColumnValueAst(
        "column", "row", "value", i32Type, types.StringView);

    TLocation loc{};
    auto i64Type = std::make_shared<TIntegerType>();
    auto boolType = std::make_shared<TBoolType>();
    auto columnType = std::make_shared<TNamedType>("TColumn", types.Column);
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "column",
            std::make_shared<TReferenceType>(columnType)),
        std::make_shared<TVarStmt>(loc, "row", i64Type),
        std::make_shared<TVarStmt>(loc, "valid",
            std::make_shared<TPointerType>(boolType)),
    };
    std::vector<TExprPtr> body = std::move(materialized.Setup);
    body.insert(body.end(), {
        std::make_shared<TArrayAssignExpr>(loc, "valid",
            std::vector<TExprPtr>{ident("row")}, materialized.IsValid),
        std::make_shared<TReturnExpr>(loc,
            std::make_shared<TCastExpr>(loc, materialized.Value, i64Type)),
    });
    auto function = std::make_shared<TFunDecl>(loc, "read_i32_column",
        std::move(params), std::make_shared<TBlockExpr>(loc, std::move(body)),
        i64Type);
    auto program = std::make_shared<TBlockExpr>(loc,
        std::vector<TExprPtr>{std::move(function)});

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    auto runner = std::make_unique<TLLVMRunner>(options);
    runner->RegisterModule(
        std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    std::string error;
    entry = runner->CompileKernelAst(
        std::move(program), "read_i32_column", &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

template <typename T>
void CheckStringHandleJit(const std::string& typeName) {
    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.ResolveCoreInput = true;
    options.NativeCode = true;

    TLLVMRunner runner(options);
    runner.RegisterModule(
        std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);

    const std::string source =
        "(block "
        "  (fun copy_handle ((var dst <ptr " + typeName + ">) "
        "                    (var src <ptr " + typeName + ">)) -> i64 "
        "    (block "
        "      (= dst [(: 0 i64)] (index src (: 0 i64))) "
        "      (return (field (index dst (: 0 i64)) Size)))))";

    std::string error;
    void* entry = runner.CompileKernel(source, &error);
    ASSERT_NE(entry, nullptr) << error;

    uint8_t bytes[] = {'a', 0, 'b'};
    T sourceValue{.Data = bytes, .Size = 3};
    T destination{};
    auto copy = reinterpret_cast<int64_t(*)(T*, T*)>(entry);

    EXPECT_EQ(copy(&destination, &sourceValue), 3);
    EXPECT_EQ(destination.Data, bytes);
    EXPECT_EQ(destination.Size, 3);
    EXPECT_EQ(destination.Data[1], 0);
}

TEST(QumirDbStringTypes, ExternalTypesAreDistinctPodStructs) {
    NQumir::NRegistry::QumirDbModule module;
    auto stringView = FindExternalType(module, "StringView");
    auto ownedString = FindExternalType(module, "OwnedString");

    ASSERT_NE(stringView, nullptr);
    ASSERT_NE(ownedString, nullptr);
    EXPECT_NE(stringView, ownedString);
    EXPECT_FALSE(TMaybeType<TStringType>(stringView));
    EXPECT_FALSE(TMaybeType<TStringType>(ownedString));

    for (const auto& type : {stringView, ownedString}) {
        auto structure = TMaybeType<TStructType>(type);
        ASSERT_TRUE(structure);
        ASSERT_EQ(structure.Cast()->Fields.size(), 2u);
        EXPECT_EQ(structure.Cast()->Fields[0].first, "Data");
        EXPECT_TRUE(TMaybeType<TPointerType>(structure.Cast()->Fields[0].second));
        EXPECT_EQ(structure.Cast()->Fields[1].first, "Size");
        EXPECT_TRUE(TMaybeType<TIntegerType>(structure.Cast()->Fields[1].second));
    }
}

TEST(QumirDbStringTypes, StringViewUsesPlainStructCopies) {
    CheckStringHandleJit<NQqb::TStringView>("StringView");
}

TEST(QumirDbStringTypes, OwnedStringUsesPlainStructCopies) {
    CheckStringHandleJit<NQqb::TOwnedString>("OwnedString");
}

TEST(QumirDbStringTypes, MaterializesI32AndValidity) {
    void* entry = nullptr;
    auto runner = CompileI32ColumnReader(entry);
    ASSERT_NE(entry, nullptr);

    std::array<int32_t, 3> values = {17, -4, 29};
    std::array<uint8_t, 1> mask = {0b00000101};
    NQqb::TColumn column{
        .Data = reinterpret_cast<char*>(values.data()),
        .Mask = mask.data(),
    };
    std::array<bool, 3> valid{};
    auto read = reinterpret_cast<int64_t(*)(NQqb::TColumn*, int64_t, bool*)>(entry);

    EXPECT_EQ(read(&column, 0, valid.data()), 17);
    EXPECT_EQ(read(&column, 1, valid.data()), -4);
    EXPECT_EQ(read(&column, 2, valid.data()), 29);
    EXPECT_TRUE(valid[0]);
    EXPECT_FALSE(valid[1]);
    EXPECT_TRUE(valid[2]);
}

TEST(QumirDbStringTypes, MaterializesStringAndLargeStringSlices) {
    void* entry = nullptr;
    auto runner = CompileStringColumnReader(entry);
    ASSERT_NE(entry, nullptr);

    std::array<uint8_t, 1> mask = {0b00001010};
    std::array<NQqb::TStringView, 3> output{};
    std::array<bool, 3> valid{};
    auto read = reinterpret_cast<int64_t(*)(
        NQqb::TColumn*, int64_t, NQqb::TStringView*, bool*)>(entry);

    std::array<int32_t, 4> offsets32 = {10, 10, 13, 17};
    std::array<uint8_t, 7> bytes32 = {'a', 'b', 'c', 'W', 0, 'Y', 'Z'};
    NQqb::TColumn stringColumn{
        .Data = reinterpret_cast<char*>(bytes32.data()),
        .Mask = mask.data(),
        .MaskBitOffset = 1,
        .Offsets = offsets32.data(),
        .OffsetWidth = 4,
    };
    EXPECT_EQ(read(&stringColumn, 0, output.data(), valid.data()), 0);
    EXPECT_EQ(read(&stringColumn, 1, output.data(), valid.data()), 3);
    EXPECT_EQ(read(&stringColumn, 2, output.data(), valid.data()), 4);
    EXPECT_TRUE(valid[0]);
    EXPECT_FALSE(valid[1]);
    EXPECT_TRUE(valid[2]);
    EXPECT_EQ(output[1].Data, bytes32.data());
    EXPECT_EQ(output[2].Data, bytes32.data() + 3);
    EXPECT_EQ(output[2].Data[1], 0);

    std::array<int64_t, 3> offsets64 = {100, 102, 107};
    std::array<uint8_t, 7> bytes64 = {'o', 'k', 'h', 'e', 'l', 'l', 'o'};
    NQqb::TColumn largeStringColumn{
        .Data = reinterpret_cast<char*>(bytes64.data()),
        .Offsets = offsets64.data(),
        .OffsetWidth = 8,
    };
    EXPECT_EQ(read(&largeStringColumn, 0, output.data(), valid.data()), 2);
    EXPECT_EQ(read(&largeStringColumn, 1, output.data(), valid.data()), 5);
    EXPECT_TRUE(valid[0]);
    EXPECT_TRUE(valid[1]);
    EXPECT_EQ(output[0].Data, bytes64.data());
    EXPECT_EQ(output[1].Data, bytes64.data() + 2);
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
