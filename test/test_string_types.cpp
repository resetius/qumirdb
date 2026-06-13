#include <gtest/gtest.h>

#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_types.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>

#include <cstdint>
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

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
