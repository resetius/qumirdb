#include <gtest/gtest.h>

#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/gen.h>
#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/modules/qumirdb_types.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>

#include <memory>
#include <string>

namespace {

using namespace NQumir::NAst;

std::shared_ptr<TStructType> StructOf(const TTypePtr& type) {
    auto named = TMaybeType<TNamedType>(type);
    auto concrete = named ? named.Cast()->UnderlyingType : type;
    auto structure = TMaybeType<TStructType>(concrete);
    return structure ? structure.Cast() : nullptr;
}

std::string NamedTypeName(const TTypePtr& type) {
    auto named = TMaybeType<TNamedType>(type);
    return named ? named.Cast()->Name : std::string{};
}

std::unique_ptr<NQumir::TLLVMRunner> CompileKeyOperation(
    const NQqb::NKernel::TAggregateKeyDescriptor& key,
    const std::string& entryName,
    const std::string& operation,
    const std::vector<TTypePtr>& paramTypes,
    const TTypePtr& resultType,
    void*& entry,
    bool pointerParams = false)
{
    using namespace NQumir;
    using namespace NQumir::NAst;

    std::vector<TExprPtr> stmts;
    if (!key.LookupTypeName.empty()) {
        stmts.push_back(std::make_shared<TTypeDeclStmt>(
            TLocation{}, key.LookupType));
    }
    if (!key.StoredTypeName.empty()) {
        stmts.push_back(std::make_shared<TTypeDeclStmt>(
            TLocation{}, key.StoredType));
    }
    auto stringOps = NQqb::NKernel::ParseFunctionLibrary(
        NQqb::NKernel::ReadAggregationKernel("string_ops.oz"));
    if (!stringOps) {
        ADD_FAILURE() << stringOps.error().ToString();
        return {};
    }
    stmts.insert(stmts.end(), stringOps->begin(), stringOps->end());
    auto keyOps = NQqb::NKernel::GenKeyOperationFunDecls(key);
    stmts.insert(stmts.end(), keyOps.begin(), keyOps.end());

    std::vector<TParam> params;
    std::vector<TExprPtr> args;
    std::vector<TTypePtr> wrapperParamTypes;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        const std::string name = "arg_" + std::to_string(i);
        TTypePtr wrapperType = pointerParams
            ? std::make_shared<TPointerType>(paramTypes[i])
            : paramTypes[i];
        params.push_back(std::make_shared<TVarStmt>(
            TLocation{}, name, wrapperType));
        wrapperParamTypes.push_back(wrapperType);
        TExprPtr argument = std::make_shared<TIdentExpr>(TLocation{}, name);
        args.push_back(pointerParams
            ? std::make_shared<TIndexExpr>(TLocation{}, argument,
                std::make_shared<TNumberExpr>(TLocation{}, int64_t{0}))
            : argument);
    }
    auto call = std::make_shared<TCallExpr>(TLocation{},
        std::make_shared<TIdentExpr>(TLocation{}, operation), std::move(args));
    auto function = std::make_shared<TFunDecl>(TLocation{}, entryName,
        std::move(params), std::make_shared<TBlockExpr>(TLocation{},
            std::vector<TExprPtr>{
                std::make_shared<TReturnExpr>(TLocation{}, std::move(call))}),
        resultType);
    function->Type = std::make_shared<TFunctionType>(
        wrapperParamTypes, resultType);
    stmts.push_back(std::move(function));

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<TLLVMRunner>(options);
    runner->RegisterModule(
        std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    auto program = std::make_shared<TBlockExpr>(TLocation{}, std::move(stmts));
    std::string error;
    entry = runner->CompileKernelAst(program, entryName, &error);
    EXPECT_NE(entry, nullptr) << error;
    return runner;
}

TEST(StringKeyDescriptor, BuildsScalarLookupAndStoredTypes) {
    TStructType input({{"name", std::make_shared<TStringType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"name"});

    ASSERT_TRUE(key.IsScalar());
    EXPECT_TRUE(key.HasDistinctLookupType());
    EXPECT_EQ(NamedTypeName(key.LookupType), "StringView");
    EXPECT_EQ(NamedTypeName(key.StoredType), "OwnedString");
    EXPECT_EQ(key.KeyType, key.StoredType);
    EXPECT_EQ(key.Size, 16u);
    EXPECT_EQ(key.Alignment, 8u);
    ASSERT_EQ(key.Fields.size(), 1u);
    EXPECT_TRUE(TMaybeType<TStringType>(key.Fields[0].Type));
    EXPECT_EQ(key.Fields[0].LookupType, key.LookupType);
    EXPECT_EQ(key.Fields[0].StoredType, key.StoredType);
}

TEST(StringKeyDescriptor, BuildsParallelCompositeLayouts) {
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto string = std::make_shared<TStringType>();
    TStructType input({{"id", i64}, {"name", string}, {"code", i32}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(
        input, {"id", "name", "code"});

    ASSERT_FALSE(key.IsScalar());
    EXPECT_TRUE(key.HasDistinctLookupType());
    EXPECT_EQ(NamedTypeName(key.LookupType), key.TypeName + "_Lookup");
    EXPECT_EQ(NamedTypeName(key.StoredType), key.TypeName + "_Stored");
    EXPECT_EQ(key.Size, 32u);
    EXPECT_EQ(key.Alignment, 8u);
    EXPECT_EQ(key.Fields[0].Offset, 0u);
    EXPECT_EQ(key.Fields[1].Offset, 8u);
    EXPECT_EQ(key.Fields[2].Offset, 24u);

    auto lookup = StructOf(key.LookupType);
    auto stored = StructOf(key.StoredType);
    ASSERT_NE(lookup, nullptr);
    ASSERT_NE(stored, nullptr);
    ASSERT_EQ(lookup->Fields.size(), stored->Fields.size());
    for (size_t i = 0; i < lookup->Fields.size(); ++i) {
        EXPECT_EQ(lookup->Fields[i].first, stored->Fields[i].first);
    }
    EXPECT_EQ(NamedTypeName(lookup->Fields[1].second), "StringView");
    EXPECT_EQ(NamedTypeName(stored->Fields[1].second), "OwnedString");
}

TEST(StringKeyDescriptor, RewritesNestedStringLeaves) {
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto nested = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"prefix", i64}, {"text", std::make_shared<TStringType>()}});
    TStructType input({{"nested", nested}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"nested"});

    ASSERT_TRUE(key.IsScalar());
    EXPECT_TRUE(key.HasDistinctLookupType());
    EXPECT_EQ(NamedTypeName(key.LookupType), key.TypeName + "_Lookup");
    EXPECT_EQ(NamedTypeName(key.StoredType), key.TypeName + "_Stored");
    auto lookup = StructOf(key.LookupType);
    auto stored = StructOf(key.StoredType);
    ASSERT_NE(lookup, nullptr);
    ASSERT_NE(stored, nullptr);
    ASSERT_EQ(lookup->Fields.size(), 2u);
    ASSERT_EQ(stored->Fields.size(), 2u);
    EXPECT_EQ(NamedTypeName(lookup->Fields[1].second), "StringView");
    EXPECT_EQ(NamedTypeName(stored->Fields[1].second), "OwnedString");
    EXPECT_EQ(key.Size, 24u);
    EXPECT_EQ(key.Alignment, 8u);
}

TEST(StringKeyDescriptor, KeepsFixedWidthRepresentationShared) {
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType input({{"first", i64}, {"second", i64}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(
        input, {"first", "second"});

    EXPECT_FALSE(key.HasDistinctLookupType());
    EXPECT_EQ(key.LookupType, key.StoredType);
    EXPECT_EQ(key.KeyType, key.StoredType);
}

TEST(StringKeyDescriptor, CompilesScalarCrossRepresentationOperations) {
    TStructType input({{"name", std::make_shared<TStringType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"name"});
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto boolean = std::make_shared<TBoolType>();
    std::string bytes("same\0bytes", 10);
    NQqb::TStringView lookup{
        .Data = reinterpret_cast<uint8_t*>(bytes.data()),
        .Size = static_cast<int64_t>(bytes.size()),
    };
    NQqb::TOwnedString stored{.Data = lookup.Data, .Size = lookup.Size};

    void* lookupHashEntry = nullptr;
    auto lookupHashRunner = CompileKeyOperation(key, "hash_lookup", "rh_hash",
        {key.LookupType}, i64, lookupHashEntry);
    ASSERT_NE(lookupHashEntry, nullptr);
    void* storedHashEntry = nullptr;
    auto storedHashRunner = CompileKeyOperation(key, "hash_stored", "rh_hash",
        {key.StoredType}, i64, storedHashEntry);
    ASSERT_NE(storedHashEntry, nullptr);
    void* equalEntry = nullptr;
    auto equalRunner = CompileKeyOperation(key, "equal_stored_lookup",
        "rh_key_equal", {key.StoredType, key.LookupType}, boolean, equalEntry);
    ASSERT_NE(equalEntry, nullptr);

    auto lookupHash = reinterpret_cast<int64_t(*)(NQqb::TStringView)>(
        lookupHashEntry);
    auto storedHash = reinterpret_cast<int64_t(*)(NQqb::TOwnedString)>(
        storedHashEntry);
    auto equal = reinterpret_cast<bool(*)(
        NQqb::TOwnedString, NQqb::TStringView)>(equalEntry);
    EXPECT_EQ(lookupHash(lookup), storedHash(stored));
    EXPECT_TRUE(equal(stored, lookup));

    std::string different = "different";
    auto differentLookup = NQqb::TStringView{
        .Data = reinterpret_cast<uint8_t*>(different.data()),
        .Size = static_cast<int64_t>(different.size()),
    };
    EXPECT_FALSE(equal(stored, differentLookup));
}

struct TLookupPair {
    int64_t Id;
    NQqb::TStringView Name;
};

struct TStoredPair {
    int64_t Id;
    NQqb::TOwnedString Name;
};

static_assert(sizeof(TLookupPair) == 24);
static_assert(sizeof(TStoredPair) == 24);

TEST(StringKeyDescriptor, CompilesCompositeCrossRepresentationOperations) {
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    TStructType input({{"id", i64}, {"name", std::make_shared<TStringType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"id", "name"});
    auto boolean = std::make_shared<TBoolType>();

    void* equalEntry = nullptr;
    auto runner = CompileKeyOperation(key, "equal_pair", "rh_key_equal",
        {key.StoredType, key.LookupType}, boolean, equalEntry, true);
    ASSERT_NE(equalEntry, nullptr);
    auto equal = reinterpret_cast<bool(*)(TStoredPair*, TLookupPair*)>(equalEntry);

    std::string bytes = "group";
    TLookupPair lookup{
        .Id = 7,
        .Name = {
            .Data = reinterpret_cast<uint8_t*>(bytes.data()),
            .Size = static_cast<int64_t>(bytes.size()),
        },
    };
    TStoredPair stored{
        .Id = lookup.Id,
        .Name = {.Data = lookup.Name.Data, .Size = lookup.Name.Size},
    };
    EXPECT_TRUE(equal(&stored, &lookup));
    lookup.Id = 8;
    EXPECT_FALSE(equal(&stored, &lookup));
}

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer initializer;
    return RUN_ALL_TESTS();
}
