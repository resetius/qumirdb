#include <gtest/gtest.h>

#include <qdb/kernel/aggregate_key.h>

#include <qumir/parser/type.h>

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

} // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
