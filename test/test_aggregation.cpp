#include <gtest/gtest.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/runner/runner_llvm.h>

#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/executor.h>
#include <qdb/exec/planner.h>
#include <qdb/io/io.h>
#include <qdb/kernel/compiler.h>
#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/gen.h>
#include <qdb/kernel/lib.h>
#include <qdb/modules/qumirdb.h>
#include <qdb/ops/aggregate.h>
#include <qdb/ops/source.h>
#include <qdb/pipeline/column_pruning.h>
#include <qdb/pipeline/typing.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

std::filesystem::path KernelDir = "qdb/kernel/aggregation";

std::string ReadKernel(const std::string& name) {
    const auto path = KernelDir / name;
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open aggregation kernel: " + path.string());
    }
    std::ostringstream source;
    source << input.rdbuf();
    return source.str();
}

std::unique_ptr<NQumir::TLLVMRunner> CompileKernel(
    const std::string& name,
    void*& entry,
    std::string& error,
    const std::unordered_set<std::string>& exclude = {})
{
    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.ResolveCoreInput = true;
    options.AllowOverloads = true;
    options.NativeCode = true;

    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);
    runner->RegisterModule(std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);

    if (exclude.empty()) {
        entry = runner->CompileKernel(ReadKernel(name), &error);
        return runner;
    }

    auto library = NQqb::NKernel::ParseFunctionLibrary(ReadKernel(name), exclude);
    if (!library) {
        error = "ParseFunctionLibrary: " + library.error().ToString();
        return runner;
    }
    if (library->empty()) {
        error = name + ": no functions remain after exclude";
        return runner;
    }
    auto funcs = std::move(*library);
    auto entryFun = std::move(funcs.back());
    funcs.pop_back();
    auto merged = NQqb::NKernel::MergeKernelLibrary(std::move(funcs), std::move(entryFun));
    entry = runner->CompileKernelAst(merged, &error);
    return runner;
}

std::unique_ptr<NQumir::TLLVMRunner> CompileGenericI64TableEntry(
    const std::string& entrySource,
    bool includeGenericOperations,
    void*& entry,
    std::string& error)
{
    using namespace NQumir;

    std::vector<NAst::TExprPtr> library;
    if (includeGenericOperations) {
        NAst::TStructType input({{"key", std::make_shared<NAst::TIntegerType>()}});
        auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});
        library = NQqb::NKernel::GenKeyOperationFunDecls(key);
    }
    for (const char* name : {"robin_hood_generic.oz", "robin_hood_rehash_generic.oz"}) {
        if (!includeGenericOperations) {
            continue;
        }
        auto parsed = NQqb::NKernel::ParseFunctionLibrary(ReadKernel(name));
        if (!parsed) {
            error = std::string(name) + ": " + parsed.error().ToString();
            return {};
        }
        library.insert(library.end(), parsed->begin(), parsed->end());
    }
    const std::unordered_set<std::string> exclude = includeGenericOperations
        ? std::unordered_set<std::string>{}
        : std::unordered_set<std::string>{
              "agg_upsert_key_bytes", "agg_rehash_key_bytes",
              "agg_upsert_grow_key_bytes"};
    auto table = NQqb::NKernel::ParseFunctionLibrary(
        ReadKernel("aggregation_table_generic.oz"), exclude);
    if (!table) {
        error = "aggregation_table_generic.oz: " + table.error().ToString();
        return {};
    }
    library.insert(library.end(), table->begin(), table->end());
    auto parsedEntry = NQqb::NKernel::ParseFunctionLibrary(entrySource);
    if (!parsedEntry || parsedEntry->size() != 1) {
        error = parsedEntry ? "generic table entry must contain one function"
                            : parsedEntry.error().ToString();
        return {};
    }

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<TLLVMRunner>(options);
    runner->RegisterModule(std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    std::vector<NAst::TExprPtr> programStmts;
    if (includeGenericOperations) {
        constexpr const char* warmupSource = R"(
(block
  (fun aggregation_generic_rehash_i64_warmup
       ((var keys_ref <ptr <ptr u8>>)
        (var dist_ref <ptr <ptr i64>>)
        (var slot_ids_ref <ptr <ptr i64>>)
        (var capacity_ref <ptr i64>)
        (var key_size i64)
        (var key i64)
        (var new_capacity i64)) -> bool
    (block
      (return (call agg_rehash_key_bytes keys_ref dist_ref slot_ids_ref
                    capacity_ref key_size key new_capacity))))
  (fun aggregation_generic_upsert_i64_warmup
       ((var keys <ptr u8>)
        (var dist <ptr i64>)
        (var slot_ids <ptr i64>)
        (var capacity i64)
        (var size <ptr i64>)
        (var key i64)
        (var out_is_new <ptr i64>)) -> i64
    (block
      (return (call agg_upsert_key_bytes keys dist slot_ids capacity size key
                    out_is_new)))))
)";
        auto warmups = NQqb::NKernel::ParseFunctionLibrary(warmupSource);
        if (!warmups || warmups->size() != 2) {
            error = "failed to parse generic table specialization warmups";
            return {};
        }

        // Pre-specialize leaves first. The runtime entry then instantiates
        // only agg_upsert_grow<Key>, making that ABI-compatible clone the last
        // lowered function returned by CompileKernelAst.
        programStmts.reserve(3 + library.size());
        programStmts.push_back(library[0]);
        programStmts.push_back(library[1]);
        programStmts.insert(programStmts.end(), warmups->begin(), warmups->end());
        programStmts.insert(programStmts.end(), library.begin() + 2, library.end());
    } else {
        programStmts = std::move(library);
    }
    programStmts.push_back(parsedEntry->front());
    auto program = std::make_shared<NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(programStmts));
    entry = runner->CompileKernelAst(program, &error);
    return runner;
}

std::unique_ptr<NQumir::TLLVMRunner> CompileGenericI64DenseEntry(
    const std::string& entrySource,
    bool genericEntry,
    void*& entry,
    std::string& error)
{
    using namespace NQumir;

    NAst::TStructType input({{"key", std::make_shared<NAst::TIntegerType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});
    auto library = NQqb::NKernel::GenKeyOperationFunDecls(key);
    auto reducers = NQqb::NKernel::GenReducerFunDecls({"sum", "count"});
    library.insert(library.end(), reducers.begin(), reducers.end());
    library.push_back(NQqb::NKernel::GenApplyReducersFunDecl(2));

    auto dense = NQqb::NKernel::ParseFunctionLibrary(
        ReadKernel("aggregation_dense_generic.oz"));
    if (!dense) {
        error = "aggregation_dense_generic.oz: " + dense.error().ToString();
        return {};
    }
    auto parsedEntry = NQqb::NKernel::ParseFunctionLibrary(entrySource);
    if (!parsedEntry || parsedEntry->size() != 1) {
        error = parsedEntry ? "generic dense entry must contain one function"
                            : parsedEntry.error().ToString();
        return {};
    }

    std::vector<NAst::TExprPtr> stmts = std::move(library);
    if (genericEntry) {
        stmts.push_back(parsedEntry->front());
        stmts.insert(stmts.end(), dense->begin(), dense->end());
    } else {
        stmts.insert(stmts.end(), dense->begin(), dense->end());
        stmts.push_back(parsedEntry->front());
    }

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<TLLVMRunner>(options);
    runner->RegisterModule(std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    auto program = std::make_shared<NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(stmts));
    entry = runner->CompileKernelAst(program, &error);
    return runner;
}

std::unique_ptr<NQumir::TLLVMRunner> CompileGenericI64FullUpdate(
    const std::string& entrySource,
    void*& entry,
    std::string& error)
{
    using namespace NQumir;

    NAst::TStructType input({{"key", std::make_shared<NAst::TIntegerType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});
    auto stmts = NQqb::NKernel::GenKeyOperationFunDecls(key);
    auto reducers = NQqb::NKernel::GenReducerFunDecls({"sum", "count"});
    stmts.insert(stmts.end(), reducers.begin(), reducers.end());
    stmts.push_back(NQqb::NKernel::GenApplyReducersFunDecl(2));

    constexpr const char* warmupSource = R"(
(block
  (fun warmup_rehash ((var keys_ref <ptr <ptr u8>>)
                      (var dist_ref <ptr <ptr i64>>)
                      (var slot_ids_ref <ptr <ptr i64>>)
                      (var capacity_ref <ptr i64>)
                      (var key_size i64)
                      (var key i64)
                      (var new_capacity i64)) -> bool
    (block
      (return (call agg_rehash_key_bytes keys_ref dist_ref slot_ids_ref
                    capacity_ref key_size key new_capacity))))
  (fun warmup_upsert ((var keys <ptr u8>)
                      (var dist <ptr i64>)
                      (var slot_ids <ptr i64>)
                      (var capacity i64)
                      (var size <ptr i64>)
                      (var key i64)
                      (var out_is_new <ptr i64>)) -> i64
    (block
      (return (call agg_upsert_key_bytes keys dist slot_ids capacity size key
                    out_is_new))))
  (fun warmup_dense_grow ((var group_keys_ref <ptr <ptr u8>>)
                          (var agg_buffers_ref <ptr <ptr <ptr i64>>>)
                          (var old_capacity i64)
                          (var new_capacity i64)
                          (var size i64)
                          (var key_size i64)
                          (var num_aggs i64)
                          (var key i64)) -> bool
    (block
      (return (call agg_dense_grow group_keys_ref agg_buffers_ref old_capacity
                    new_capacity size key_size num_aggs key))))
  (fun warmup_dense_update ((var group_keys <ptr u8>)
                            (var agg_buffers <ptr <ptr i64>>)
                            (var dense_slot i64)
                            (var key i64)
                            (var value i64)
                            (var is_new bool))
    (block
      (call agg_dense_update group_keys agg_buffers dense_slot key value
            is_new))))
)";
    auto warmups = NQqb::NKernel::ParseFunctionLibrary(warmupSource);
    if (!warmups) {
        error = warmups.error().ToString();
        return {};
    }
    stmts.insert(stmts.end(), warmups->begin(), warmups->end());

    auto parsedEntry = NQqb::NKernel::ParseFunctionLibrary(entrySource);
    if (!parsedEntry || parsedEntry->size() != 1) {
        error = parsedEntry ? "full update entry must contain one function"
                            : parsedEntry.error().ToString();
        return {};
    }
    stmts.push_back(parsedEntry->front());

    for (const char* name : {
             "robin_hood_generic.oz",
             "robin_hood_rehash_generic.oz",
             "aggregation_table_generic.oz",
             "aggregation_dense_generic.oz",
             "aggregation_update_generic.oz"}) {
        auto parsed = NQqb::NKernel::ParseFunctionLibrary(ReadKernel(name));
        if (!parsed) {
            error = std::string(name) + ": " + parsed.error().ToString();
            return {};
        }
        stmts.insert(stmts.end(), parsed->begin(), parsed->end());
    }

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<TLLVMRunner>(options);
    runner->RegisterModule(std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    auto program = std::make_shared<NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(stmts));
    entry = runner->CompileKernelAst(program, &error);
    return runner;
}

// Builds reduce_smoke(i64 prev, i64 value, i64 is_new_flag, <ptr i64> out),
// which for i in [0, numReducers) does
//   out[i] = reduce_i(prev, value, is_new_flag != 0)
// — numReducers static, direct, by-name calls to the reduce_0..reduce_{N-1}
// functions generated by GenReducerFunDecls. This mirrors the pattern the
// future generated agg_update will use to update N aggregate buffers per
// row, with no function pointers and no dispatch on Func.
NQumir::NAst::TExprPtr GenReduceSmokeEntry(size_t numReducers) {
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto boolType = std::make_shared<TBoolType>();
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);

    auto ident = [&](const std::string& name) {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto numI64 = [&](int64_t value) -> TExprPtr {
        auto expr = std::make_shared<TNumberExpr>(loc, value);
        expr->Type = i64Type;
        return expr;
    };

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "prev", i64Type),
        std::make_shared<TVarStmt>(loc, "value", i64Type),
        std::make_shared<TVarStmt>(loc, "is_new_flag", i64Type),
        std::make_shared<TVarStmt>(loc, "out", ptrI64Type),
    };

    std::vector<TExprPtr> stmts;
    stmts.push_back(std::make_shared<TVarStmt>(loc, "is_new", boolType));
    stmts.push_back(std::make_shared<TAssignExpr>(loc, "is_new",
        std::make_shared<TBinaryExpr>(loc, TOperator("!="), ident("is_new_flag"), numI64(0))));

    for (size_t i = 0; i < numReducers; ++i) {
        auto call = std::make_shared<TCallExpr>(loc,
            ident("reduce_" + std::to_string(i)),
            std::vector<TExprPtr>{ident("prev"), ident("value"), ident("is_new")});
        stmts.push_back(std::make_shared<TArrayAssignExpr>(loc, "out",
            std::vector<TExprPtr>{numI64(static_cast<int64_t>(i))}, call));
    }

    auto body = std::make_shared<TBlockExpr>(loc, std::move(stmts));
    return std::make_shared<TFunDecl>(loc, "reduce_smoke", std::move(params), body,
        std::make_shared<TVoidType>());
}

// Generates reduce_0..reduce_{N-1} (GenReducerFunDecls) for `funcs`, merges
// them with a reduce_smoke entry (GenReduceSmokeEntry) via L1, and compiles
// the result. No HashTable/TRowSet involved; this exercises only
// the predefined-name reducer generation + static AST injection.
std::unique_ptr<NQumir::TLLVMRunner> CompileReducerSmoke(
    const std::vector<std::string>& funcs,
    void*& entry,
    std::string& error)
{
    auto reducers = NQqb::NKernel::GenReducerFunDecls(funcs);
    auto smokeEntry = GenReduceSmokeEntry(funcs.size());

    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;

    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);

    auto merged = NQqb::NKernel::MergeKernelLibrary(std::move(reducers), std::move(smokeEntry));
    entry = runner->CompileKernelAst(merged, &error);
    return runner;
}

// Generates apply_smoke(<ptr <ptr i64>> agg_buffers, i64 dense_slot,
// i64 value, i64 is_new_flag), which calls
//   agg_apply_reducers(agg_buffers, dense_slot, value, is_new_flag != 0)
// — the single per-query generated function (GenApplyReducersFunDecl) that
// the future NumAggs-generic agg_update (L2b) will call to update all N
// aggregate buffers for one dense slot.
NQumir::NAst::TExprPtr GenApplyReducersSmokeEntry() {
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto boolType = std::make_shared<TBoolType>();
    auto ptrI64Type = std::make_shared<TPointerType>(i64Type);
    auto ptrPtrI64Type = std::make_shared<TPointerType>(ptrI64Type);

    auto ident = [&](const std::string& name) {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto numI64 = [&](int64_t value) -> TExprPtr {
        auto expr = std::make_shared<TNumberExpr>(loc, value);
        expr->Type = i64Type;
        return expr;
    };

    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "agg_buffers", ptrPtrI64Type),
        std::make_shared<TVarStmt>(loc, "dense_slot", i64Type),
        std::make_shared<TVarStmt>(loc, "value", i64Type),
        std::make_shared<TVarStmt>(loc, "is_new_flag", i64Type),
    };

    std::vector<TExprPtr> stmts;
    stmts.push_back(std::make_shared<TVarStmt>(loc, "is_new", boolType));
    stmts.push_back(std::make_shared<TAssignExpr>(loc, "is_new",
        std::make_shared<TBinaryExpr>(loc, TOperator("!="), ident("is_new_flag"), numI64(0))));
    stmts.push_back(std::make_shared<TCallExpr>(loc, ident("agg_apply_reducers"),
        std::vector<TExprPtr>{ident("agg_buffers"), ident("dense_slot"), ident("value"), ident("is_new")}));

    auto body = std::make_shared<TBlockExpr>(loc, std::move(stmts));
    return std::make_shared<TFunDecl>(loc, "apply_smoke", std::move(params), body,
        std::make_shared<TVoidType>());
}

// Generates reduce_0..reduce_{N-1} (GenReducerFunDecls) plus
// agg_apply_reducers (GenApplyReducersFunDecl) for `funcs`, merges them with
// an apply_smoke entry (GenApplyReducersSmokeEntry) via L1, and compiles the
// result. No HashTable/TRowSet involved; exercises the single
// per-query generated function that updates all N aggregate buffers for one
// dense slot, by static name, with no function pointers.
std::unique_ptr<NQumir::TLLVMRunner> CompileApplyReducersSmoke(
    const std::vector<std::string>& funcs,
    void*& entry,
    std::string& error)
{
    auto library = NQqb::NKernel::GenReducerFunDecls(funcs);
    library.push_back(NQqb::NKernel::GenApplyReducersFunDecl(funcs.size()));
    auto smokeEntry = GenApplyReducersSmokeEntry();

    NQumir::TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;

    auto runner = std::make_unique<NQumir::TLLVMRunner>(options);

    auto merged = NQqb::NKernel::MergeKernelLibrary(std::move(library), std::move(smokeEntry));
    entry = runner->CompileKernelAst(merged, &error);
    return runner;
}

struct THashTable {
    uint8_t* Keys = nullptr;
    int64_t* Dist = nullptr;
    int64_t* SlotId = nullptr;
    uint8_t* GroupKeys = nullptr;
    int64_t** AggBuffers = nullptr;
    uint8_t* Scratch = nullptr;
    uint8_t* Scratch2 = nullptr;
    uint8_t* QueryKey = nullptr;
    int64_t Capacity = 0;
    int64_t Size = 0;
    int64_t NumAggs = 0;
    int64_t NumKeys = 0;
    int64_t KeySize = 0;
};

struct TPairI64Key {
    int64_t First;
    int64_t Second;

    bool operator==(const TPairI64Key&) const = default;
};

struct TNestedI64Key {
    int64_t First;
    TPairI64Key Nested;
};

static_assert(sizeof(THashTable) == 104);
static_assert(offsetof(THashTable, Keys) == 0);
static_assert(offsetof(THashTable, GroupKeys) == 24);
static_assert(offsetof(THashTable, QueryKey) == 56);
static_assert(offsetof(THashTable, Capacity) == 64);
static_assert(offsetof(THashTable, KeySize) == 96);

int64_t* I64Keys(THashTable& table) {
    return reinterpret_cast<int64_t*>(table.Keys);
}

const int64_t* I64Keys(const THashTable& table) {
    return reinterpret_cast<const int64_t*>(table.Keys);
}

int64_t* I64GroupKeys(THashTable& table) {
    return reinterpret_cast<int64_t*>(table.GroupKeys);
}

const int64_t* I64GroupKeys(const THashTable& table) {
    return reinterpret_cast<const int64_t*>(table.GroupKeys);
}

uint64_t HashI64(int64_t key) {
    uint64_t h = static_cast<uint64_t>(key);
    h ^= h >> 12;
    h ^= h << 25;
    h ^= h >> 27;
    return h * UINT64_C(2685821657736338717);
}

uint64_t HashCombineStep(uint64_t seed, uint64_t fieldHash) {
    return seed ^ (fieldHash + UINT64_C(0x9e3779b97f4a7c15) +
        (seed << 6) + (seed >> 2));
}

uint64_t HashCombineI64(std::initializer_list<int64_t> fields) {
    uint64_t seed = 0;
    for (int64_t field : fields) {
        seed = HashCombineStep(seed, HashI64(field));
    }
    return seed;
}

std::vector<int64_t> KeysForHome(int64_t home, int64_t capacity, size_t count) {
    std::vector<int64_t> result;
    for (int64_t key = 0; result.size() < count; ++key) {
        if (static_cast<int64_t>(HashI64(key) & static_cast<uint64_t>(capacity - 1)) == home) {
            result.push_back(key);
        }
    }
    return result;
}

} // namespace

TEST(AggregationKeyDescriptor, BuildsScalarAndCompositeFixedWidthLayouts) {
    using namespace NQumir::NAst;

    auto i8 = std::make_shared<TIntegerType>(TIntegerType::I8);
    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto u64 = std::make_shared<TIntegerType>(TIntegerType::U64);
    auto f64 = std::make_shared<TFloatType>();
    TStructType input({
        {"small", i8}, {"id", i64}, {"id2", i64}, {"code", i32},
        {"unsigned_id", u64}, {"score", f64}});

    auto scalar = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"code"});
    ASSERT_TRUE(scalar.IsScalar());
    EXPECT_EQ(scalar.KeyType, i32);
    EXPECT_EQ(scalar.Size, 4);
    EXPECT_EQ(scalar.Alignment, 4);
    ASSERT_EQ(scalar.Fields.size(), 1u);
    EXPECT_EQ(scalar.Fields[0].ColumnIndex, 3);
    EXPECT_EQ(scalar.Fields[0].Offset, 0);

    auto i64Pair = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"id", "id2"});
    EXPECT_EQ(i64Pair.Size, 16);
    EXPECT_EQ(i64Pair.Alignment, 8);
    EXPECT_EQ(i64Pair.Fields[0].Offset, 0);
    EXPECT_EQ(i64Pair.Fields[1].Offset, 8);

    auto pair = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"id", "score"});
    EXPECT_FALSE(pair.IsScalar());
    EXPECT_EQ(pair.Size, 16);
    EXPECT_EQ(pair.Alignment, 8);
    ASSERT_EQ(pair.Fields.size(), 2u);
    EXPECT_EQ(pair.Fields[0].ColumnIndex, 1);
    EXPECT_EQ(pair.Fields[0].Offset, 0);
    EXPECT_EQ(pair.Fields[1].ColumnIndex, 5);
    EXPECT_EQ(pair.Fields[1].Offset, 8);
    auto named = TMaybeType<TNamedType>(pair.KeyType);
    ASSERT_TRUE(named);
    EXPECT_EQ(named.Cast()->Name, pair.TypeName);
    EXPECT_NE(pair.TypeName, i64Pair.TypeName);
    auto pairStruct = TMaybeType<TStructType>(named.Cast()->UnderlyingType);
    ASSERT_TRUE(pairStruct);
    ASSERT_EQ(pairStruct.Cast()->Fields.size(), 2u);
    EXPECT_EQ(pairStruct.Cast()->Fields[0].first, "key_0");
    EXPECT_EQ(pairStruct.Cast()->Fields[1].first, "key_1");

    auto mixed = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"code", "score"});
    EXPECT_EQ(mixed.Fields[0].Offset, 0);
    EXPECT_EQ(mixed.Fields[1].Offset, 8);
    EXPECT_EQ(mixed.Size, 16);
    auto mixedNamed = TMaybeType<TNamedType>(mixed.KeyType);
    ASSERT_TRUE(mixedNamed);
    auto mixedStruct = TMaybeType<TStructType>(mixedNamed.Cast()->UnderlyingType);
    ASSERT_TRUE(mixedStruct);
    ASSERT_EQ(mixedStruct.Cast()->Fields.size(), 6u);
    EXPECT_EQ(mixedStruct.Cast()->Fields[0].first, "key_0");
    EXPECT_EQ(mixedStruct.Cast()->Fields[1].first, "__qdb_padding_0");
    EXPECT_EQ(mixedStruct.Cast()->Fields[4].first, "__qdb_padding_3");
    EXPECT_EQ(mixedStruct.Cast()->Fields[5].first, "key_1");

    auto padded = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"small", "id"});
    EXPECT_EQ(padded.Fields[0].Offset, 0);
    EXPECT_EQ(padded.Fields[1].Offset, 8);
    EXPECT_EQ(padded.Size, 16);
    EXPECT_EQ(padded.Alignment, 8);

    auto unsignedScalar = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"unsigned_id"});
    EXPECT_EQ(unsignedScalar.KeyType, u64);
    EXPECT_EQ(unsignedScalar.Size, 8);
}

TEST(AggregationKeyDescriptor, RejectsMissingVariableWidthAndEmptyKeys) {
    using namespace NQumir::NAst;

    TStructType input({
        {"id", std::make_shared<TIntegerType>()},
        {"name", std::make_shared<TStringType>()},
    });
    EXPECT_THROW(NQqb::NKernel::BuildAggregateKeyDescriptor(input, {}), NQumir::TError);
    EXPECT_THROW(NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"missing"}), NQumir::TError);
    EXPECT_THROW(NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"name"}), NQumir::TError);
}

TEST(AggregationKernel, GenericKeyDispatchInstantiatesI64) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("generic_key_smoke.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TSmokeFn = int64_t(*)(int64_t);
    auto smoke = reinterpret_cast<TSmokeFn>(entry);
    EXPECT_EQ(smoke(0), 0);
    EXPECT_EQ(smoke(42), 42);
    EXPECT_EQ(smoke(-17), -17);
}

TEST(AggregationKernel, MergedInjectedI64KeyOpsDriveGenericRobinHoodUpsert) {
    using namespace NQumir;

    NAst::TStructType input({{"key", std::make_shared<NAst::TIntegerType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});
    auto keyOps = NQqb::NKernel::GenKeyOperationFunDecls(key);
    ASSERT_EQ(keyOps.size(), 2u);
    auto robinHood = NQqb::NKernel::ParseFunctionLibrary(ReadKernel("robin_hood_generic.oz"));
    ASSERT_TRUE(robinHood) << robinHood.error().ToString();

    constexpr const char* entrySource = R"(
(block
  (fun aggregation_generic_upsert_i64 ((var keys <ptr i64>)
                                       (var dist <ptr i64>)
                                       (var slot_ids <ptr i64>)
                                       (var capacity i64)
                                       (var size <ptr i64>)
                                       (var key i64)
                                       (var out_is_new <ptr i64>)) -> i64
    (block
      (return (call rh_upsert keys dist slot_ids capacity size key out_is_new)))))
)";
    auto entry = NQqb::NKernel::ParseFunctionLibrary(entrySource);
    ASSERT_TRUE(entry) << entry.error().ToString();
    ASSERT_EQ(entry->size(), 1u);

    std::vector<NAst::TExprPtr> library;
    library.insert(library.end(), keyOps.begin(), keyOps.end());
    library.insert(library.end(), robinHood->begin(), robinHood->end());
    auto program = NQqb::NKernel::MergeKernelLibrary(std::move(library), entry->front());

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    TLLVMRunner runner(options);
    runner.RegisterModule(std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    std::string error;
    void* fn = runner.CompileKernelAst(program, &error);
    ASSERT_NE(fn, nullptr) << error;

    using TUpsertFn = int64_t(*)(
        int64_t*, int64_t*, int64_t*, int64_t, int64_t*, int64_t, int64_t*);
    auto upsert = reinterpret_cast<TUpsertFn>(fn);
    constexpr int64_t capacity = 8;
    std::array<int64_t, capacity> keys{};
    std::array<int64_t, capacity> dist{};
    std::array<int64_t, capacity> slotIds{};
    dist.fill(-1);
    slotIds.fill(-1);
    int64_t size = 0;
    int64_t isNew = -1;

    EXPECT_EQ(upsert(keys.data(), dist.data(), slotIds.data(), capacity,
                     &size, 10, &isNew), 0);
    EXPECT_EQ(isNew, 1);
    EXPECT_EQ(size, 1);
    EXPECT_EQ(upsert(keys.data(), dist.data(), slotIds.data(), capacity,
                     &size, 20, &isNew), 1);
    EXPECT_EQ(isNew, 1);
    EXPECT_EQ(upsert(keys.data(), dist.data(), slotIds.data(), capacity,
                     &size, 10, &isNew), 0);
    EXPECT_EQ(isNew, 0);
    EXPECT_EQ(size, 2);
}

TEST(AggregationKernel, MergedInjectedI64KeyOpsDriveGenericRobinHoodRehash) {
    using namespace NQumir;

    NAst::TStructType input({{"key", std::make_shared<NAst::TIntegerType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});
    auto keyOps = NQqb::NKernel::GenKeyOperationFunDecls(key);
    ASSERT_EQ(keyOps.size(), 2u);
    auto robinHood = NQqb::NKernel::ParseFunctionLibrary(
        ReadKernel("robin_hood_rehash_generic.oz"));
    ASSERT_TRUE(robinHood) << robinHood.error().ToString();

    constexpr const char* entrySource = R"(
(block
  (fun aggregation_generic_rehash_i64 ((var old_keys <ptr i64>)
                                       (var old_dist <ptr i64>)
                                       (var old_slot_ids <ptr i64>)
                                       (var old_capacity i64)
                                       (var new_keys <ptr i64>)
                                       (var new_dist <ptr i64>)
                                       (var new_slot_ids <ptr i64>)
                                       (var new_capacity i64)) -> bool
    (block
      (return (call rh_rehash_into old_keys old_dist old_slot_ids old_capacity
                                   new_keys new_dist new_slot_ids new_capacity)))))
)";
    auto entry = NQqb::NKernel::ParseFunctionLibrary(entrySource);
    ASSERT_TRUE(entry) << entry.error().ToString();
    ASSERT_EQ(entry->size(), 1u);

    std::vector<NAst::TExprPtr> library;
    library.insert(library.end(), keyOps.begin(), keyOps.end());
    library.insert(library.end(), robinHood->begin(), robinHood->end());
    auto program = NQqb::NKernel::MergeKernelLibrary(std::move(library), entry->front());

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    TLLVMRunner runner(options);
    std::string error;
    void* fn = runner.CompileKernelAst(program, &error);
    ASSERT_NE(fn, nullptr) << error;

    using TRehashFn = bool(*)(
        int64_t*, int64_t*, int64_t*, int64_t,
        int64_t*, int64_t*, int64_t*, int64_t);
    auto rehash = reinterpret_cast<TRehashFn>(fn);

    constexpr int64_t oldCapacity = 8;
    constexpr int64_t newCapacity = 16;
    std::array<int64_t, oldCapacity> oldKeys{};
    std::array<int64_t, oldCapacity> oldDist{};
    std::array<int64_t, oldCapacity> oldSlotIds{};
    oldDist.fill(-1);
    oldSlotIds.fill(-1);

    // Three occupied consecutive slots model a valid collision chain. Rehash
    // must preserve the dense ids even though physical slots can change.
    oldKeys[2] = 10;
    oldDist[2] = 0;
    oldSlotIds[2] = 4;
    oldKeys[3] = 20;
    oldDist[3] = 1;
    oldSlotIds[3] = 1;
    oldKeys[4] = 30;
    oldDist[4] = 2;
    oldSlotIds[4] = 7;

    std::array<int64_t, newCapacity> newKeys{};
    std::array<int64_t, newCapacity> newDist{};
    std::array<int64_t, newCapacity> newSlotIds{};
    ASSERT_TRUE(rehash(oldKeys.data(), oldDist.data(), oldSlotIds.data(), oldCapacity,
                       newKeys.data(), newDist.data(), newSlotIds.data(), newCapacity));

    std::array<bool, 3> found{};
    for (int64_t slot = 0; slot < newCapacity; ++slot) {
        if (newDist[slot] == -1) {
            continue;
        }
        if (newKeys[slot] == 10) {
            EXPECT_EQ(newSlotIds[slot], 4);
            found[0] = true;
        } else if (newKeys[slot] == 20) {
            EXPECT_EQ(newSlotIds[slot], 1);
            found[1] = true;
        } else if (newKeys[slot] == 30) {
            EXPECT_EQ(newSlotIds[slot], 7);
            found[2] = true;
        } else {
            ADD_FAILURE() << "unexpected key after rehash: " << newKeys[slot];
        }
    }
    EXPECT_TRUE(found[0]);
    EXPECT_TRUE(found[1]);
    EXPECT_TRUE(found[2]);
}

TEST(AggregationKernel, OpaqueByteKeyStorageIsCastOnlyInsideGenericTableAst) {
    using namespace NQumir;

    NAst::TStructType input({{"key", std::make_shared<NAst::TIntegerType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});
    auto keyOps = NQqb::NKernel::GenKeyOperationFunDecls(key);
    auto robinHood = NQqb::NKernel::ParseFunctionLibrary(ReadKernel("robin_hood_generic.oz"));
    ASSERT_TRUE(robinHood) << robinHood.error().ToString();
    auto table = NQqb::NKernel::ParseFunctionLibrary(
        ReadKernel("aggregation_table_generic.oz"), {
            "agg_table_init_bytes", "agg_table_destroy_bytes",
            "agg_rehash_key_bytes", "agg_upsert_grow_key_bytes"});
    ASSERT_TRUE(table) << table.error().ToString();

    constexpr const char* entrySource = R"(
(block
  (fun aggregation_byte_storage_i64 ((var key_bytes <ptr u8>)
                                     (var dist <ptr i64>)
                                     (var slot_ids <ptr i64>)
                                     (var capacity i64)
                                     (var size <ptr i64>)
                                     (var key i64)
                                     (var out_is_new <ptr i64>)) -> i64
    (block
      (return (call agg_upsert_key_bytes key_bytes dist slot_ids capacity size
                                         key out_is_new)))))
)";
    auto entry = NQqb::NKernel::ParseFunctionLibrary(entrySource);
    ASSERT_TRUE(entry) << entry.error().ToString();

    std::vector<NAst::TExprPtr> library;
    library.insert(library.end(), keyOps.begin(), keyOps.end());
    library.insert(library.end(), robinHood->begin(), robinHood->end());
    library.insert(library.end(), table->begin(), table->end());
    auto program = NQqb::NKernel::MergeKernelLibrary(std::move(library), entry->front());

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    TLLVMRunner runner(options);
    runner.RegisterModule(std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    std::string error;
    void* fn = runner.CompileKernelAst(program, &error);
    ASSERT_NE(fn, nullptr) << error;

    using TUpsertFn = int64_t(*)(
        uint8_t*, int64_t*, int64_t*, int64_t, int64_t*, int64_t, int64_t*);
    auto upsert = reinterpret_cast<TUpsertFn>(fn);
    constexpr int64_t capacity = 8;
    std::array<uint8_t, capacity * sizeof(int64_t)> keyBytes{};
    std::array<int64_t, capacity> dist{};
    std::array<int64_t, capacity> slotIds{};
    dist.fill(-1);
    slotIds.fill(-1);
    int64_t size = 0;
    int64_t isNew = -1;

    EXPECT_EQ(upsert(keyBytes.data(), dist.data(), slotIds.data(), capacity,
                     &size, 42, &isNew), 0);
    EXPECT_EQ(isNew, 1);
    EXPECT_EQ(upsert(keyBytes.data(), dist.data(), slotIds.data(), capacity,
                     &size, 42, &isNew), 0);
    EXPECT_EQ(isNew, 0);
    EXPECT_EQ(size, 1);

    bool found = false;
    for (int64_t slot = 0; slot < capacity; ++slot) {
        if (dist[slot] == -1) {
            continue;
        }
        int64_t stored = 0;
        std::memcpy(&stored, keyBytes.data() + slot * sizeof(stored), sizeof(stored));
        EXPECT_EQ(stored, 42);
        EXPECT_EQ(slotIds[slot], 0);
        found = true;
    }
    EXPECT_TRUE(found);
}

TEST(AggregationKernel, GenericOpaqueTableLifecycleGrowsAndDestroys) {
    constexpr const char* initSource = R"(
(block
  (fun aggregation_generic_table_init ((var keys_out <ptr <ptr u8>>)
                                       (var dist_out <ptr <ptr i64>>)
                                       (var slot_ids_out <ptr <ptr i64>>)
                                       (var capacity_out <ptr i64>)
                                       (var size_out <ptr i64>)
                                       (var capacity i64)
                                       (var key_size i64)) -> bool
    (block
      (return (call agg_table_init_bytes keys_out dist_out slot_ids_out
                    capacity_out size_out capacity key_size)))))
)";
    constexpr const char* upsertSource = R"(
(block
  (fun aggregation_generic_table_upsert ((var keys_ref <ptr <ptr u8>>)
                                         (var dist_ref <ptr <ptr i64>>)
                                         (var slot_ids_ref <ptr <ptr i64>>)
                                         (var capacity_ref <ptr i64>)
                                         (var size_ref <ptr i64>)
                                         (var key_size i64)
                                         (var key i64)
                                         (var out_is_new <ptr i64>)) -> i64
    (block
      (return (call agg_upsert_grow_key_bytes keys_ref dist_ref slot_ids_ref
                    capacity_ref size_ref key_size key out_is_new)))))
)";
    constexpr const char* destroySource = R"(
(block
  (fun aggregation_generic_table_destroy ((var keys_ref <ptr <ptr u8>>)
                                          (var dist_ref <ptr <ptr i64>>)
                                          (var slot_ids_ref <ptr <ptr i64>>)
                                          (var capacity_ref <ptr i64>)
                                          (var size_ref <ptr i64>)) -> i64
    (block
      (call agg_table_destroy_bytes keys_ref dist_ref slot_ids_ref
            capacity_ref size_ref)
      (return (: 1 i64)))))
)";

    void* initEntry = nullptr;
    void* upsertEntry = nullptr;
    void* destroyEntry = nullptr;
    std::string error;
    auto initRunner = CompileGenericI64TableEntry(
        initSource, false, initEntry, error);
    ASSERT_NE(initRunner, nullptr) << error;
    ASSERT_NE(initEntry, nullptr) << error;
    auto upsertRunner = CompileGenericI64TableEntry(
        upsertSource, true, upsertEntry, error);
    ASSERT_NE(upsertRunner, nullptr) << error;
    ASSERT_NE(upsertEntry, nullptr) << error;
    auto destroyRunner = CompileGenericI64TableEntry(
        destroySource, false, destroyEntry, error);
    ASSERT_NE(destroyRunner, nullptr) << error;
    ASSERT_NE(destroyEntry, nullptr) << error;

    using TInitFn = bool(*)(
        uint8_t**, int64_t**, int64_t**, int64_t*, int64_t*, int64_t, int64_t);
    using TUpsertFn = int64_t(*)(
        uint8_t**, int64_t**, int64_t**, int64_t*, int64_t*, int64_t,
        int64_t, int64_t*);
    using TDestroyFn = int64_t(*)(
        uint8_t**, int64_t**, int64_t**, int64_t*, int64_t*);
    auto init = reinterpret_cast<TInitFn>(initEntry);
    auto upsert = reinterpret_cast<TUpsertFn>(upsertEntry);
    auto destroy = reinterpret_cast<TDestroyFn>(destroyEntry);

    uint8_t* keys = nullptr;
    int64_t* dist = nullptr;
    int64_t* slotIds = nullptr;
    int64_t capacity = 0;
    int64_t size = 0;
    constexpr int64_t keySize = sizeof(int64_t);
    ASSERT_TRUE(init(&keys, &dist, &slotIds, &capacity, &size, 4, keySize));
    ASSERT_NE(keys, nullptr);
    ASSERT_NE(dist, nullptr);
    ASSERT_NE(slotIds, nullptr);
    EXPECT_EQ(capacity, 4);

    constexpr std::array<int64_t, 12> input = {
        0, 1, -1, 17, 33, 49, 65, 81, 97, 113, 129, 145};
    for (size_t i = 0; i < input.size(); ++i) {
        int64_t isNew = -1;
        EXPECT_EQ(upsert(&keys, &dist, &slotIds, &capacity, &size, keySize,
                         input[i], &isNew), static_cast<int64_t>(i));
        EXPECT_EQ(isNew, 1);
        EXPECT_EQ(size, static_cast<int64_t>(i + 1));
    }
    EXPECT_EQ(capacity, 16);

    for (size_t i = 0; i < input.size(); ++i) {
        int64_t isNew = -1;
        EXPECT_EQ(upsert(&keys, &dist, &slotIds, &capacity, &size, keySize,
                         input[i], &isNew), static_cast<int64_t>(i));
        EXPECT_EQ(isNew, 0);
    }
    EXPECT_EQ(size, static_cast<int64_t>(input.size()));

    std::array<bool, input.size()> seen{};
    for (int64_t slot = 0; slot < capacity; ++slot) {
        if (dist[slot] == -1) {
            continue;
        }
        ASSERT_GE(slotIds[slot], 0);
        ASSERT_LT(slotIds[slot], size);
        EXPECT_FALSE(seen[slotIds[slot]]);
        seen[slotIds[slot]] = true;
        int64_t stored = 0;
        std::memcpy(&stored, keys + slot * keySize, sizeof(stored));
        EXPECT_EQ(stored, input[slotIds[slot]]);
    }
    for (bool present : seen) {
        EXPECT_TRUE(present);
    }

    EXPECT_EQ(destroy(&keys, &dist, &slotIds, &capacity, &size), 1);
    EXPECT_EQ(keys, nullptr);
    EXPECT_EQ(dist, nullptr);
    EXPECT_EQ(slotIds, nullptr);
    EXPECT_EQ(capacity, 0);
    EXPECT_EQ(size, 0);
}

TEST(AggregationKernel, GenericDenseStoragePreservesKeysAndReducerStatesAcrossGrow) {
    constexpr const char* initSource = R"(
(block
  (fun aggregation_dense_init_i64 ((var group_keys_out <ptr <ptr u8>>)
                                   (var agg_buffers_out <ptr <ptr <ptr i64>>>)
                                   (var capacity i64)
                                   (var key_size i64)
                                   (var num_aggs i64)) -> bool
    (block
      (return (call agg_dense_init group_keys_out agg_buffers_out capacity
                    key_size num_aggs)))))
)";
    constexpr const char* updateSource = R"(
(block
  (fun aggregation_dense_update_i64 ((var group_keys <ptr u8>)
                                     (var agg_buffers <ptr <ptr i64>>)
                                     (var dense_slot i64)
                                     (var key i64)
                                     (var value i64)
                                     (var is_new bool))
    (block
      (call agg_dense_update group_keys agg_buffers dense_slot key value
            is_new))))
)";
    constexpr const char* growSource = R"(
(block
  (fun aggregation_dense_grow_i64 ((var group_keys_ref <ptr <ptr u8>>)
                                   (var agg_buffers_ref <ptr <ptr <ptr i64>>>)
                                   (var old_capacity i64)
                                   (var new_capacity i64)
                                   (var size i64)
                                   (var key_size i64)
                                   (var num_aggs i64)
                                   (var key_witness i64)) -> bool
    (block
      (return (call agg_dense_grow group_keys_ref agg_buffers_ref old_capacity
                    new_capacity size key_size num_aggs key_witness)))))
)";
    constexpr const char* destroySource = R"(
(block
  (fun aggregation_dense_destroy_i64 ((var group_keys_ref <ptr <ptr u8>>)
                                      (var agg_buffers_ref <ptr <ptr <ptr i64>>>)
                                      (var num_aggs i64)) -> i64
    (block
      (call agg_dense_destroy group_keys_ref agg_buffers_ref num_aggs)
      (return (: 1 i64)))))
)";

    void* initEntry = nullptr;
    void* updateEntry = nullptr;
    void* growEntry = nullptr;
    void* destroyEntry = nullptr;
    std::string error;
    auto initRunner = CompileGenericI64DenseEntry(
        initSource, false, initEntry, error);
    ASSERT_NE(initEntry, nullptr) << error;
    auto updateRunner = CompileGenericI64DenseEntry(
        updateSource, true, updateEntry, error);
    ASSERT_NE(updateEntry, nullptr) << error;
    auto growRunner = CompileGenericI64DenseEntry(
        growSource, true, growEntry, error);
    ASSERT_NE(growEntry, nullptr) << error;
    auto destroyRunner = CompileGenericI64DenseEntry(
        destroySource, false, destroyEntry, error);
    ASSERT_NE(destroyEntry, nullptr) << error;

    using TInitFn = bool(*)(uint8_t**, int64_t***, int64_t, int64_t, int64_t);
    using TUpdateFn = void(*)(uint8_t*, int64_t**, int64_t, int64_t, int64_t, bool);
    using TGrowFn = bool(*)(
        uint8_t**, int64_t***, int64_t, int64_t, int64_t, int64_t, int64_t,
        int64_t);
    using TDestroyFn = int64_t(*)(uint8_t**, int64_t***, int64_t);
    auto init = reinterpret_cast<TInitFn>(initEntry);
    auto update = reinterpret_cast<TUpdateFn>(updateEntry);
    auto grow = reinterpret_cast<TGrowFn>(growEntry);
    auto destroy = reinterpret_cast<TDestroyFn>(destroyEntry);

    uint8_t* groupKeys = nullptr;
    int64_t** aggBuffers = nullptr;
    constexpr int64_t keySize = sizeof(int64_t);
    constexpr int64_t numAggs = 2;
    ASSERT_TRUE(init(&groupKeys, &aggBuffers, 4, keySize, numAggs));
    ASSERT_NE(groupKeys, nullptr);
    ASSERT_NE(aggBuffers, nullptr);

    update(groupKeys, aggBuffers, 0, 10, 7, true);
    update(groupKeys, aggBuffers, 1, 20, 3, true);
    update(groupKeys, aggBuffers, 0, 10, 5, false);
    update(groupKeys, aggBuffers, 2, -4, 11, true);
    EXPECT_EQ(aggBuffers[0][0], 12);
    EXPECT_EQ(aggBuffers[1][0], 2);
    EXPECT_EQ(aggBuffers[0][1], 3);
    EXPECT_EQ(aggBuffers[1][1], 1);
    EXPECT_EQ(aggBuffers[0][2], 11);
    EXPECT_EQ(aggBuffers[1][2], 1);

    ASSERT_TRUE(grow(&groupKeys, &aggBuffers, 4, 8, 3, keySize, numAggs, 0));
    std::array<int64_t, 3> expectedKeys = {10, 20, -4};
    for (int64_t slot = 0; slot < 3; ++slot) {
        int64_t key = 0;
        std::memcpy(&key, groupKeys + slot * keySize, sizeof(key));
        EXPECT_EQ(key, expectedKeys[slot]);
    }
    EXPECT_EQ(aggBuffers[0][0], 12);
    EXPECT_EQ(aggBuffers[1][0], 2);
    EXPECT_EQ(aggBuffers[0][1], 3);
    EXPECT_EQ(aggBuffers[1][1], 1);
    EXPECT_EQ(aggBuffers[0][2], 11);
    EXPECT_EQ(aggBuffers[1][2], 1);

    update(groupKeys, aggBuffers, 1, 20, 9, false);
    EXPECT_EQ(aggBuffers[0][1], 12);
    EXPECT_EQ(aggBuffers[1][1], 2);

    EXPECT_EQ(destroy(&groupKeys, &aggBuffers, numAggs), 1);
    EXPECT_EQ(groupKeys, nullptr);
    EXPECT_EQ(aggBuffers, nullptr);
}

TEST(AggregationKernel, GenericFullUpdateGrowsProbeAndDenseStorageTogether) {
    constexpr const char* probeInitSource = R"(
(block
  (fun full_probe_init ((var keys_out <ptr <ptr u8>>)
                        (var dist_out <ptr <ptr i64>>)
                        (var slot_ids_out <ptr <ptr i64>>)
                        (var capacity_out <ptr i64>)
                        (var size_out <ptr i64>)
                        (var capacity i64)
                        (var key_size i64)) -> bool
    (block
      (return (call agg_table_init_bytes keys_out dist_out slot_ids_out
                    capacity_out size_out capacity key_size)))))
)";
    constexpr const char* denseInitSource = R"(
(block
  (fun full_dense_init ((var group_keys_out <ptr <ptr u8>>)
                        (var agg_buffers_out <ptr <ptr <ptr i64>>>)
                        (var capacity i64)
                        (var key_size i64)
                        (var num_aggs i64)) -> bool
    (block
      (return (call agg_dense_init group_keys_out agg_buffers_out capacity
                    key_size num_aggs)))))
)";
    constexpr const char* updateSource = R"(
(block
  (fun full_update_i64 ((var keys_ref <ptr <ptr u8>>)
                        (var dist_ref <ptr <ptr i64>>)
                        (var slot_ids_ref <ptr <ptr i64>>)
                        (var group_keys_ref <ptr <ptr u8>>)
                        (var agg_buffers_ref <ptr <ptr <ptr i64>>>)
                        (var capacity_ref <ptr i64>)
                        (var size_ref <ptr i64>)
                        (var key_size i64)
                        (var num_aggs i64)
                        (var key i64)
                        (var value i64)
                        (var out_is_new <ptr i64>)) -> i64
    (block
      (return (call agg_table_update_full keys_ref dist_ref slot_ids_ref
                    group_keys_ref agg_buffers_ref capacity_ref size_ref
                    key_size num_aggs key value out_is_new)))))
)";
    constexpr const char* probeDestroySource = R"(
(block
  (fun full_probe_destroy ((var keys_ref <ptr <ptr u8>>)
                           (var dist_ref <ptr <ptr i64>>)
                           (var slot_ids_ref <ptr <ptr i64>>)
                           (var capacity_ref <ptr i64>)
                           (var size_ref <ptr i64>)) -> i64
    (block
      (call agg_table_destroy_bytes keys_ref dist_ref slot_ids_ref
            capacity_ref size_ref)
      (return (: 1 i64)))))
)";
    constexpr const char* denseDestroySource = R"(
(block
  (fun full_dense_destroy ((var group_keys_ref <ptr <ptr u8>>)
                           (var agg_buffers_ref <ptr <ptr <ptr i64>>>)
                           (var num_aggs i64)) -> i64
    (block
      (call agg_dense_destroy group_keys_ref agg_buffers_ref num_aggs)
      (return (: 1 i64)))))
)";

    void* probeInitEntry = nullptr;
    void* denseInitEntry = nullptr;
    void* updateEntry = nullptr;
    void* probeDestroyEntry = nullptr;
    void* denseDestroyEntry = nullptr;
    std::string error;
    auto probeInitRunner = CompileGenericI64TableEntry(
        probeInitSource, false, probeInitEntry, error);
    ASSERT_NE(probeInitEntry, nullptr) << error;
    auto denseInitRunner = CompileGenericI64DenseEntry(
        denseInitSource, false, denseInitEntry, error);
    ASSERT_NE(denseInitEntry, nullptr) << error;
    auto updateRunner = CompileGenericI64FullUpdate(updateSource, updateEntry, error);
    ASSERT_NE(updateEntry, nullptr) << error;
    auto probeDestroyRunner = CompileGenericI64TableEntry(
        probeDestroySource, false, probeDestroyEntry, error);
    ASSERT_NE(probeDestroyEntry, nullptr) << error;
    auto denseDestroyRunner = CompileGenericI64DenseEntry(
        denseDestroySource, false, denseDestroyEntry, error);
    ASSERT_NE(denseDestroyEntry, nullptr) << error;

    using TProbeInitFn = bool(*)(
        uint8_t**, int64_t**, int64_t**, int64_t*, int64_t*, int64_t, int64_t);
    using TDenseInitFn = bool(*)(uint8_t**, int64_t***, int64_t, int64_t, int64_t);
    using TUpdateFn = int64_t(*)(
        uint8_t**, int64_t**, int64_t**, uint8_t**, int64_t***,
        int64_t*, int64_t*, int64_t, int64_t, int64_t, int64_t, int64_t*);
    using TProbeDestroyFn = int64_t(*)(
        uint8_t**, int64_t**, int64_t**, int64_t*, int64_t*);
    using TDenseDestroyFn = int64_t(*)(uint8_t**, int64_t***, int64_t);

    uint8_t* keys = nullptr;
    int64_t* dist = nullptr;
    int64_t* slotIds = nullptr;
    uint8_t* groupKeys = nullptr;
    int64_t** aggBuffers = nullptr;
    int64_t capacity = 0;
    int64_t size = 0;
    constexpr int64_t keySize = sizeof(int64_t);
    constexpr int64_t numAggs = 2;
    ASSERT_TRUE(reinterpret_cast<TProbeInitFn>(probeInitEntry)(
        &keys, &dist, &slotIds, &capacity, &size, 4, keySize));
    ASSERT_TRUE(reinterpret_cast<TDenseInitFn>(denseInitEntry)(
        &groupKeys, &aggBuffers, capacity, keySize, numAggs));
    auto update = reinterpret_cast<TUpdateFn>(updateEntry);

    constexpr std::array<int64_t, 10> inputKeys =
        {7, 9, 7, -3, 11, 9, 13, 15, -3, 17};
    constexpr std::array<int64_t, 10> inputValues =
        {5, 2, 8, 4, 1, 6, 3, 7, 10, 9};
    std::unordered_map<int64_t, std::pair<int64_t, int64_t>> expected;
    std::unordered_map<int64_t, int64_t> denseSlots;
    for (size_t i = 0; i < inputKeys.size(); ++i) {
        int64_t isNew = -1;
        const int64_t dense = update(
            &keys, &dist, &slotIds, &groupKeys, &aggBuffers,
            &capacity, &size, keySize, numAggs, inputKeys[i], inputValues[i],
            &isNew);
        ASSERT_GE(dense, 0);
        auto [it, inserted] = denseSlots.emplace(inputKeys[i], dense);
        EXPECT_EQ(isNew, inserted ? 1 : 0);
        EXPECT_EQ(dense, it->second);
        expected[inputKeys[i]].first += inputValues[i];
        expected[inputKeys[i]].second += 1;
    }
    EXPECT_EQ(capacity, 16);
    EXPECT_EQ(size, static_cast<int64_t>(expected.size()));

    for (const auto& [key, dense] : denseSlots) {
        int64_t storedKey = 0;
        std::memcpy(&storedKey, groupKeys + dense * keySize, sizeof(storedKey));
        EXPECT_EQ(storedKey, key);
        EXPECT_EQ(aggBuffers[0][dense], expected[key].first);
        EXPECT_EQ(aggBuffers[1][dense], expected[key].second);
    }

    EXPECT_EQ(reinterpret_cast<TDenseDestroyFn>(denseDestroyEntry)(
        &groupKeys, &aggBuffers, numAggs), 1);
    EXPECT_EQ(reinterpret_cast<TProbeDestroyFn>(probeDestroyEntry)(
        &keys, &dist, &slotIds, &capacity, &size), 1);
}

// Drives the production-shaped generic <ref HashTable> table library
// (aggregation_hashtable_generic.oz) for Key=i64 via a single op-dispatched
// concrete entry, selected by name through the explicit-entry CompileKernelAst
// overload (so appended generic specializations cannot steal the entry point).
TEST(AggregationKernel, GenericRefHashTableUpsertsGrowsAndAggregatesI64) {
    using namespace NQumir;

    NAst::TStructType input({{"key", std::make_shared<NAst::TIntegerType>()}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(input, {"key"});
    auto stmts = NQqb::NKernel::GenKeyOperationFunDecls(key);
    auto reducers = NQqb::NKernel::GenReducerFunDecls({"sum", "count"});
    stmts.insert(stmts.end(), reducers.begin(), reducers.end());
    stmts.push_back(NQqb::NKernel::GenApplyReducersFunDecl(2));

    for (const char* name : {"robin_hood_rehash_generic.oz",
                             "aggregation_hashtable_generic.oz"}) {
        auto parsed = NQqb::NKernel::ParseFunctionLibrary(ReadKernel(name));
        ASSERT_TRUE(parsed.has_value())
            << name << ": " << parsed.error().ToString();
        stmts.insert(stmts.end(), parsed->begin(), parsed->end());
    }

    constexpr const char* entrySource = R"(
(block
  (fun aht_drive ((var ht <ref HashTable>)
                      (var a i64)
                      (var b i64)
                      (var op i64)) -> i64
    (block
      (return
        (if (== op (: 0 i64))
          (if (call aht_init ht a b (: 8 i64)) (: 1 i64) (: 0 i64))
          (if (== op (: 1 i64))
            (call aht_update ht a b)
            (block (call aht_destroy ht) (: 1 i64))))))))
)";
    auto parsedEntry = NQqb::NKernel::ParseFunctionLibrary(entrySource);
    ASSERT_TRUE(parsedEntry.has_value());
    ASSERT_EQ(parsedEntry->size(), 1u);
    stmts.push_back(parsedEntry->front());

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    auto runner = std::make_unique<TLLVMRunner>(options);
    runner->RegisterModule(std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
    auto program = std::make_shared<NAst::TBlockExpr>(
        NQumir::TLocation{}, std::move(stmts));
    std::string error;
    void* entry = runner->CompileKernelAst(program, "aht_drive", &error);
    ASSERT_NE(entry, nullptr) << error;

    using TDriveFn = int64_t(*)(THashTable*, int64_t, int64_t, int64_t);
    auto drive = reinterpret_cast<TDriveFn>(entry);

    THashTable table;
    ASSERT_NE(drive(&table, 4, 2, 0), 0);  // init capacity=4, num_aggs=2 (success != 0)
    EXPECT_EQ(table.KeySize, static_cast<int64_t>(sizeof(int64_t)));
    EXPECT_EQ(table.Capacity, 4);
    EXPECT_EQ(table.NumAggs, 2);

    constexpr std::array<int64_t, 10> inputKeys =
        {7, 9, 7, -3, 11, 9, 13, 15, -3, 17};
    constexpr std::array<int64_t, 10> inputValues =
        {5, 2, 8, 4, 1, 6, 3, 7, 10, 9};
    std::unordered_map<int64_t, std::pair<int64_t, int64_t>> expected;  // sum, count
    std::unordered_map<int64_t, int64_t> denseSlots;
    for (size_t i = 0; i < inputKeys.size(); ++i) {
        const int64_t dense = drive(&table, inputKeys[i], inputValues[i], 1);
        ASSERT_GE(dense, 0);
        auto [it, inserted] = denseSlots.emplace(inputKeys[i], dense);
        EXPECT_EQ(dense, it->second);
        expected[inputKeys[i]].first += inputValues[i];
        expected[inputKeys[i]].second += 1;
    }
    EXPECT_EQ(table.Size, static_cast<int64_t>(expected.size()));
    EXPECT_EQ(table.Capacity, 16);

    for (const auto& [k, dense] : denseSlots) {
        EXPECT_EQ(I64GroupKeys(table)[dense], k);
        EXPECT_EQ(table.AggBuffers[0][dense], expected[k].first);   // sum
        EXPECT_EQ(table.AggBuffers[1][dense], expected[k].second);  // count
    }

    EXPECT_EQ(drive(&table, 0, 0, 2), 1);  // destroy
    EXPECT_EQ(table.Capacity, 0);
    EXPECT_EQ(table.Size, 0);
}

TEST(AggregationKernel, GenericProgramBuilderDispatchesSelectedRowSets) {
    using namespace NQumir;
    using namespace NQumir::NAst;

    auto i64Type = std::make_shared<TIntegerType>();
    TStructType inputType({{"k", i64Type}, {"v", i64Type}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(inputType, {"k"});

    auto dbModule = std::make_shared<NQumir::NRegistry::QumirDbModule>();
    TTypePtr columnType;
    TTypePtr rowSetType;
    TTypePtr hashTableType;
    for (const auto& external : dbModule->ExternalTypes()) {
        if (external.Name == "TColumn") columnType = external.Type;
        else if (external.Name == "TRowSet") rowSetType = external.Type;
        else if (external.Name == "HashTable") hashTableType = external.Type;
    }
    ASSERT_NE(columnType, nullptr);
    ASSERT_NE(rowSetType, nullptr);
    ASSERT_NE(hashTableType, nullptr);

    auto program = NQqb::NKernel::BuildGenericAggregateProgramAst(
        inputType, key, std::string("v"), {"sum", "count"},
        columnType, rowSetType, hashTableType);
    ASSERT_TRUE(program.has_value()) << program.error().ToString();

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    TLLVMRunner runner(options);
    runner.RegisterModule(dbModule, true);
    std::string error;
    void* entry = runner.CompileKernelAst(*program, "agg_dispatch", &error);
    ASSERT_NE(entry, nullptr) << error;

    using TDispatchFn = int64_t(*)(THashTable*, NQqb::TRowSet*, int64_t, int64_t);
    auto dispatch = reinterpret_cast<TDispatchFn>(entry);
    THashTable table;
    ASSERT_NE(dispatch(&table, nullptr, 4, 0), 0);
    EXPECT_EQ(table.KeySize, 8);
    EXPECT_EQ(table.NumAggs, 2);

    std::unordered_map<int64_t, std::pair<int64_t, int64_t>> expected;
    auto runBatch = [&](std::vector<int64_t>& keys,
                        std::vector<int64_t>& values,
                        std::vector<uint8_t>* selection) {
        NQqb::TColumn columns[2] = {};
        columns[0].Data = reinterpret_cast<char*>(keys.data());
        columns[1].Data = reinterpret_cast<char*>(values.data());
        NQqb::TRowSet batch{};
        batch.Columns = columns;
        batch.ColumnCount = 2;
        batch.RowCount = static_cast<int64_t>(keys.size());
        batch.Selection = selection ? selection->data() : nullptr;
        EXPECT_EQ(dispatch(&table, &batch, 0, 1), 0);
        for (size_t i = 0; i < keys.size(); ++i) {
            if (selection && !(*selection)[i]) continue;
            expected[keys[i]].first += values[i];
            expected[keys[i]].second += 1;
        }
    };

    std::vector<int64_t> keys1 = {1, 2, 1, 3, 4, 5};
    std::vector<int64_t> values1 = {10, 20, 5, 7, 11, 13};
    std::vector<uint8_t> selection1 = {1, 0, 1, 1, 0, 1};
    runBatch(keys1, values1, &selection1);

    std::vector<int64_t> keys2 = {2, 6, 3, 7, 8, 1};
    std::vector<int64_t> values2 = {4, 8, -2, 9, 12, 6};
    runBatch(keys2, values2, nullptr);

    ASSERT_EQ(table.Size, static_cast<int64_t>(expected.size()));
    ASSERT_GE(table.Capacity, 16);
    for (int64_t dense = 0; dense < table.Size; ++dense) {
        const int64_t k = I64GroupKeys(table)[dense];
        auto it = expected.find(k);
        ASSERT_NE(it, expected.end()) << "unexpected key " << k;
        EXPECT_EQ(table.AggBuffers[0][dense], it->second.first) << "key " << k;
        EXPECT_EQ(table.AggBuffers[1][dense], it->second.second) << "key " << k;
    }

    EXPECT_EQ(dispatch(&table, nullptr, 0, 2), 1);
    EXPECT_EQ(table.Keys, nullptr);
    EXPECT_EQ(table.GroupKeys, nullptr);
    EXPECT_EQ(table.Capacity, 0);
}

TEST(AggregationKernel, GenericProgramBuilderSupportsI32KeysThroughGrowAndFinalize) {
    using namespace NQumir;
    using namespace NQumir::NAst;

    auto i32Type = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto i64Type = std::make_shared<TIntegerType>();
    TStructType inputType({{"k", i32Type}, {"v", i64Type}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(inputType, {"k"});

    auto dbModule = std::make_shared<NQumir::NRegistry::QumirDbModule>();
    TTypePtr columnType;
    TTypePtr rowSetType;
    TTypePtr hashTableType;
    for (const auto& external : dbModule->ExternalTypes()) {
        if (external.Name == "TColumn") columnType = external.Type;
        else if (external.Name == "TRowSet") rowSetType = external.Type;
        else if (external.Name == "HashTable") hashTableType = external.Type;
    }
    ASSERT_NE(columnType, nullptr);
    ASSERT_NE(rowSetType, nullptr);
    ASSERT_NE(hashTableType, nullptr);

    auto updateProgram = NQqb::NKernel::BuildGenericAggregateProgramAst(
        inputType, key, std::string("v"), {"sum", "count"},
        columnType, rowSetType, hashTableType);
    ASSERT_TRUE(updateProgram.has_value()) << updateProgram.error().ToString();
    auto finalizeProgram = NQqb::NKernel::BuildGenericAggregateFinalizeProgramAst(
        key, hashTableType);
    ASSERT_TRUE(finalizeProgram.has_value()) << finalizeProgram.error().ToString();

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    TLLVMRunner updateRunner(options);
    updateRunner.RegisterModule(dbModule, true);
    TLLVMRunner finalizeRunner(options);
    finalizeRunner.RegisterModule(dbModule, true);

    std::string error;
    void* updateEntry = updateRunner.CompileKernelAst(
        *updateProgram, "agg_dispatch", &error);
    ASSERT_NE(updateEntry, nullptr) << error;
    error.clear();
    void* finalizeEntry = finalizeRunner.CompileKernelAst(
        *finalizeProgram, "agg_finalize", &error);
    ASSERT_NE(finalizeEntry, nullptr) << error;

    using TDispatchFn = int64_t(*)(THashTable*, NQqb::TRowSet*, int64_t, int64_t);
    using TFinalizeFn = int64_t(*)(THashTable*, void**, int64_t**, int64_t);
    auto dispatch = reinterpret_cast<TDispatchFn>(updateEntry);
    auto finalize = reinterpret_cast<TFinalizeFn>(finalizeEntry);

    THashTable table;
    ASSERT_NE(dispatch(&table, nullptr, 4, 0), 0);
    EXPECT_EQ(table.KeySize, sizeof(int32_t));

    std::unordered_map<int32_t, std::pair<int64_t, int64_t>> expected;
    auto runBatch = [&](std::vector<int32_t>& keys,
                        std::vector<int64_t>& values,
                        std::vector<uint8_t>* selection) {
        NQqb::TColumn columns[2] = {};
        columns[0].Data = reinterpret_cast<char*>(keys.data());
        columns[1].Data = reinterpret_cast<char*>(values.data());
        NQqb::TRowSet batch{};
        batch.Columns = columns;
        batch.ColumnCount = 2;
        batch.RowCount = static_cast<int64_t>(keys.size());
        batch.Selection = selection ? selection->data() : nullptr;
        EXPECT_EQ(dispatch(&table, &batch, 0, 1), 0);
        for (size_t i = 0; i < keys.size(); ++i) {
            if (selection && !(*selection)[i]) continue;
            expected[keys[i]].first += values[i];
            expected[keys[i]].second += 1;
        }
    };

    std::vector<int32_t> keys1 = {-1, 2, -1, 3, 4, 5};
    std::vector<int64_t> values1 = {10, 20, 5, 7, 11, 13};
    std::vector<uint8_t> selection1 = {1, 0, 1, 1, 0, 1};
    runBatch(keys1, values1, &selection1);
    std::vector<int32_t> keys2 = {
        2, 6, 3, 7, 8, std::numeric_limits<int32_t>::min()};
    std::vector<int64_t> values2 = {4, 8, -2, 9, 12, 6};
    runBatch(keys2, values2, nullptr);

    ASSERT_EQ(table.Size, static_cast<int64_t>(expected.size()));
    ASSERT_GE(table.Capacity, 16);
    std::vector<int32_t> outputKeys(table.Size);
    std::vector<int64_t> outputSums(table.Size);
    std::vector<int64_t> outputCounts(table.Size);
    void* outputKeyBuffers[] = {outputKeys.data()};
    int64_t* outputBuffers[] = {outputSums.data(), outputCounts.data()};
    ASSERT_EQ(finalize(
        &table, outputKeyBuffers, outputBuffers, outputKeys.size()), table.Size);
    for (int64_t i = 0; i < table.Size; ++i) {
        auto it = expected.find(outputKeys[i]);
        ASSERT_NE(it, expected.end()) << "unexpected key " << outputKeys[i];
        EXPECT_EQ(outputSums[i], it->second.first) << "key " << outputKeys[i];
        EXPECT_EQ(outputCounts[i], it->second.second) << "key " << outputKeys[i];
    }

    EXPECT_EQ(dispatch(&table, nullptr, 0, 2), 1);
}

TEST(AggregationKernel, GeneratedIntegerHashContractIsStable) {
    using namespace NQumir;
    using namespace NQumir::NAst;

    auto i64Type = std::make_shared<TIntegerType>();
    TStructType inputType({{"key", i64Type}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(inputType, {"key"});
    auto operations = NQqb::NKernel::GenKeyOperationFunDecls(key);
    auto program = std::make_shared<TBlockExpr>(TLocation{}, std::move(operations));

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    TLLVMRunner runner(options);
    std::string error;
    void* entry = runner.CompileKernelAst(program, "rh_hash", &error);
    ASSERT_NE(entry, nullptr) << error;

    auto hash = reinterpret_cast<int64_t(*)(int64_t)>(entry);
    EXPECT_EQ(static_cast<uint64_t>(hash(0)), UINT64_C(0x0000000000000000));
    EXPECT_EQ(static_cast<uint64_t>(hash(1)), UINT64_C(0x47e4ce4b896cdd1d));
    EXPECT_EQ(static_cast<uint64_t>(hash(-1)), UINT64_C(0xf92cc9e5c6000000));
    EXPECT_EQ(static_cast<uint64_t>(hash(7)), HashI64(7));
}

TEST(AggregationKernel, GeneratedStructKeyOperationsWalkFieldsRecursively) {
    using namespace NQumir;
    using namespace NQumir::NAst;

    auto i64Type = std::make_shared<TIntegerType>();
    TStructType inputType({{"first", i64Type}, {"second", i64Type}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(
        inputType, {"first", "second"});
    ASSERT_FALSE(key.IsScalar());

    auto compileEntry = [&](const std::string& entryName,
                            std::unique_ptr<TLLVMRunner>& runner,
                            std::string& error) -> void* {
        auto operations = NQqb::NKernel::GenKeyOperationFunDecls(key);
        operations.insert(operations.begin(),
            std::make_shared<TTypeDeclStmt>(TLocation{}, key.KeyType));
        auto program = std::make_shared<TBlockExpr>(
            TLocation{}, std::move(operations));
        TLLVMRunnerOptions options;
        options.CoreInput = true;
        options.NativeCode = true;
        options.AllowOverloads = true;
        runner = std::make_unique<TLLVMRunner>(options);
        return runner->CompileKernelAst(program, entryName, &error);
    };

    std::string error;
    std::unique_ptr<TLLVMRunner> hashRunner;
    void* hashEntry = compileEntry("rh_hash", hashRunner, error);
    ASSERT_NE(hashEntry, nullptr) << error;
    error.clear();
    std::unique_ptr<TLLVMRunner> equalRunner;
    void* equalEntry = compileEntry("rh_key_equal", equalRunner, error);
    ASSERT_NE(equalEntry, nullptr) << error;

    using THashFn = int64_t(*)(TPairI64Key);
    using TEqualFn = bool(*)(TPairI64Key, TPairI64Key);
    auto hash = reinterpret_cast<THashFn>(hashEntry);
    auto equal = reinterpret_cast<TEqualFn>(equalEntry);

    EXPECT_TRUE(equal({7, 11}, {7, 11}));
    EXPECT_FALSE(equal({7, 11}, {7, 12}));
    EXPECT_FALSE(equal({7, 11}, {8, 11}));
    EXPECT_EQ(static_cast<uint64_t>(hash({7, 11})),
        UINT64_C(0xe364af588ba97749));
    EXPECT_EQ(static_cast<uint64_t>(hash({11, 7})),
        UINT64_C(0x406ed7f5a37183d3));
    EXPECT_EQ(static_cast<uint64_t>(hash({7, 12})),
        UINT64_C(0xe5a932ba60dba6fb));
    EXPECT_EQ(static_cast<uint64_t>(hash({7, 11})), HashCombineI64({7, 11}));
    EXPECT_NE(hash({7, 11}), hash({11, 7}));

    std::unique_ptr<TLLVMRunner> secondHashRunner;
    error.clear();
    void* secondHashEntry = compileEntry("rh_hash", secondHashRunner, error);
    ASSERT_NE(secondHashEntry, nullptr) << error;
    auto secondHash = reinterpret_cast<THashFn>(secondHashEntry);
    EXPECT_EQ(hash({7, 11}), secondHash({7, 11}));
    EXPECT_EQ(hash({11, 7}), secondHash({11, 7}));
}

TEST(AggregationKernel, GeneratedNestedStructHashPreservesTypeShape) {
    using namespace NQumir;
    using namespace NQumir::NAst;

    auto i64Type = std::make_shared<TIntegerType>();
    auto nestedType = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"second", i64Type}, {"third", i64Type}});
    auto keyType = std::make_shared<TStructType>(
        std::vector<std::pair<std::string, TTypePtr>>{
            {"first", i64Type}, {"nested", nestedType}});
    TStructType inputType({{"key", keyType}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(inputType, {"key"});
    ASSERT_TRUE(key.IsScalar());
    ASSERT_EQ(key.Size, sizeof(TNestedI64Key));

    auto operations = NQqb::NKernel::GenKeyOperationFunDecls(key);
    TLocation loc{};
    auto ident = [&](const std::string& name) -> TExprPtr {
        return std::make_shared<TIdentExpr>(loc, name);
    };
    auto nested = std::make_shared<TStructConstructExpr>(loc, nestedType,
        std::vector<TExprPtr>{ident("second"), ident("third")});
    auto outer = std::make_shared<TStructConstructExpr>(loc, keyType,
        std::vector<TExprPtr>{ident("first"), std::move(nested)});
    auto call = std::make_shared<TCallExpr>(loc, ident("rh_hash"),
        std::vector<TExprPtr>{std::move(outer)});
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "first", i64Type),
        std::make_shared<TVarStmt>(loc, "second", i64Type),
        std::make_shared<TVarStmt>(loc, "third", i64Type),
    };
    auto wrapper = std::make_shared<TFunDecl>(loc, "hash_nested", params,
        std::make_shared<TBlockExpr>(loc,
            std::vector<TExprPtr>{std::make_shared<TReturnExpr>(loc, call)}),
        i64Type);
    wrapper->Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{i64Type, i64Type, i64Type}, i64Type);
    operations.push_back(std::move(wrapper));
    auto program = std::make_shared<TBlockExpr>(loc, std::move(operations));
    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    TLLVMRunner runner(options);
    std::string error;
    void* entry = runner.CompileKernelAst(program, "hash_nested", &error);
    ASSERT_NE(entry, nullptr) << error;

    auto hash = reinterpret_cast<int64_t(*)(int64_t, int64_t, int64_t)>(entry);
    const uint64_t nestedHash = HashCombineI64({11, 13});
    const uint64_t expected = HashCombineStep(
        HashCombineStep(0, HashI64(7)), nestedHash);
    EXPECT_EQ(nestedHash, UINT64_C(0x4e5714dd8ac8e8ab));
    EXPECT_EQ(expected, UINT64_C(0x65568e194da3aaf3));
    EXPECT_EQ(static_cast<uint64_t>(hash(7, 11, 13)), expected);
    EXPECT_NE(static_cast<uint64_t>(hash(7, 11, 13)),
        HashCombineI64({7, 11, 13}));
}

TEST(AggregationKernel, GeneratedF64KeyOperationsCanonicalizeZeroAndNaN) {
    using namespace NQumir;
    using namespace NQumir::NAst;

    auto f64Type = std::make_shared<TFloatType>();
    TStructType inputType({{"key", f64Type}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(inputType, {"key"});

    auto compileEntry = [&](const std::string& entryName,
                            std::unique_ptr<TLLVMRunner>& runner,
                            std::string& error) -> void* {
        auto operations = NQqb::NKernel::GenKeyOperationFunDecls(key);
        auto program = std::make_shared<TBlockExpr>(TLocation{}, std::move(operations));
        TLLVMRunnerOptions options;
        options.CoreInput = true;
        options.NativeCode = true;
        options.AllowOverloads = true;
        runner = std::make_unique<TLLVMRunner>(options);
        runner->RegisterModule(
            std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);
        return runner->CompileKernelAst(program, entryName, &error);
    };

    std::string error;
    std::unique_ptr<TLLVMRunner> hashRunner;
    void* hashEntry = compileEntry("rh_hash", hashRunner, error);
    ASSERT_NE(hashEntry, nullptr) << error;
    error.clear();
    std::unique_ptr<TLLVMRunner> equalRunner;
    void* equalEntry = compileEntry("rh_key_equal", equalRunner, error);
    ASSERT_NE(equalEntry, nullptr) << error;

    auto hash = reinterpret_cast<int64_t(*)(double)>(hashEntry);
    auto equal = reinterpret_cast<bool(*)(double, double)>(equalEntry);
    const double nan1 = std::numeric_limits<double>::quiet_NaN();
    const double nan2 = std::bit_cast<double>(UINT64_C(0x7ff0000000000001));

    EXPECT_TRUE(equal(0.0, -0.0));
    EXPECT_EQ(hash(0.0), hash(-0.0));
    EXPECT_TRUE(equal(nan1, nan2));
    EXPECT_EQ(hash(nan1), hash(nan2));
    EXPECT_FALSE(equal(nan1, 1.0));
    EXPECT_FALSE(equal(1.0, 2.0));
    EXPECT_NE(hash(1.0), hash(2.0));
}

TEST(AggregationKernel, GenericOzLibrariesSpecializeForKeyTypeMatrix) {
    using namespace NQumir;
    using namespace NQumir::NAst;

    struct TCase {
        std::string Name;
        std::vector<std::pair<std::string, TTypePtr>> Fields;
        std::vector<std::string> GroupKeys;
        size_t ExpectedKeySize;
    };

    auto i32 = std::make_shared<TIntegerType>(TIntegerType::I32);
    auto i64 = std::make_shared<TIntegerType>(TIntegerType::I64);
    auto f64 = std::make_shared<TFloatType>();
    const std::vector<TCase> cases = {
        {"i32", {{"k", i32}, {"v", i64}}, {"k"}, 4},
        {"i64", {{"k", i64}, {"v", i64}}, {"k"}, 8},
        {"f64", {{"k", f64}, {"v", i64}}, {"k"}, 8},
        {"i64_i64", {{"k1", i64}, {"k2", i64}, {"v", i64}},
            {"k1", "k2"}, 16},
        {"i64_f64", {{"k1", i64}, {"k2", f64}, {"v", i64}},
            {"k1", "k2"}, 16},
    };

    auto dbModule = std::make_shared<NQumir::NRegistry::QumirDbModule>();
    TTypePtr columnType;
    TTypePtr rowSetType;
    TTypePtr hashTableType;
    for (const auto& external : dbModule->ExternalTypes()) {
        if (external.Name == "TColumn") columnType = external.Type;
        else if (external.Name == "TRowSet") rowSetType = external.Type;
        else if (external.Name == "HashTable") hashTableType = external.Type;
    }
    ASSERT_NE(columnType, nullptr);
    ASSERT_NE(rowSetType, nullptr);
    ASSERT_NE(hashTableType, nullptr);

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    using TDispatchFn = int64_t(*)(THashTable*, NQqb::TRowSet*, int64_t, int64_t);

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.Name);
        TStructType inputType(testCase.Fields);
        auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(
            inputType, testCase.GroupKeys);
        ASSERT_EQ(key.Size, testCase.ExpectedKeySize);

        auto updateProgram = NQqb::NKernel::BuildGenericAggregateProgramAst(
            inputType, key, std::string("v"), {"sum", "count"},
            columnType, rowSetType, hashTableType);
        ASSERT_TRUE(updateProgram.has_value())
            << updateProgram.error().ToString();
        auto finalizeProgram = NQqb::NKernel::BuildGenericAggregateFinalizeProgramAst(
            key, hashTableType);
        ASSERT_TRUE(finalizeProgram.has_value())
            << finalizeProgram.error().ToString();

        TLLVMRunner updateRunner(options);
        updateRunner.RegisterModule(dbModule, true);
        TLLVMRunner finalizeRunner(options);
        finalizeRunner.RegisterModule(dbModule, true);
        std::string error;
        void* updateEntry = updateRunner.CompileKernelAst(
            *updateProgram, "agg_dispatch", &error);
        ASSERT_NE(updateEntry, nullptr) << error;
        error.clear();
        void* finalizeEntry = finalizeRunner.CompileKernelAst(
            *finalizeProgram, "agg_finalize", &error);
        ASSERT_NE(finalizeEntry, nullptr) << error;

        auto dispatch = reinterpret_cast<TDispatchFn>(updateEntry);
        THashTable table;
        ASSERT_NE(dispatch(&table, nullptr, 4, 0), 0);
        EXPECT_EQ(table.KeySize, static_cast<int64_t>(testCase.ExpectedKeySize));
        EXPECT_EQ(dispatch(&table, nullptr, 0, 2), 1);
    }
}

TEST(AggregationKernel, GenericProgramBuilderMaterializesCompositeKeysAndGrows) {
    using namespace NQumir;
    using namespace NQumir::NAst;

    auto i64Type = std::make_shared<TIntegerType>();
    TStructType inputType({
        {"k1", i64Type}, {"k2", i64Type}, {"v", i64Type}});
    auto key = NQqb::NKernel::BuildAggregateKeyDescriptor(
        inputType, {"k1", "k2"});
    ASSERT_FALSE(key.IsScalar());
    ASSERT_EQ(key.Size, sizeof(TPairI64Key));

    auto dbModule = std::make_shared<NQumir::NRegistry::QumirDbModule>();
    TTypePtr columnType;
    TTypePtr rowSetType;
    TTypePtr hashTableType;
    for (const auto& external : dbModule->ExternalTypes()) {
        if (external.Name == "TColumn") columnType = external.Type;
        else if (external.Name == "TRowSet") rowSetType = external.Type;
        else if (external.Name == "HashTable") hashTableType = external.Type;
    }

    auto updateProgram = NQqb::NKernel::BuildGenericAggregateProgramAst(
        inputType, key, std::string("v"), {"sum", "count"},
        columnType, rowSetType, hashTableType);
    ASSERT_TRUE(updateProgram.has_value()) << updateProgram.error().ToString();
    auto finalizeProgram = NQqb::NKernel::BuildGenericAggregateFinalizeProgramAst(
        key, hashTableType);
    ASSERT_TRUE(finalizeProgram.has_value()) << finalizeProgram.error().ToString();

    TLLVMRunnerOptions options;
    options.CoreInput = true;
    options.NativeCode = true;
    options.AllowOverloads = true;
    TLLVMRunner updateRunner(options);
    updateRunner.RegisterModule(dbModule, true);
    TLLVMRunner finalizeRunner(options);
    finalizeRunner.RegisterModule(dbModule, true);
    std::string error;
    void* updateEntry = updateRunner.CompileKernelAst(
        *updateProgram, "agg_dispatch", &error);
    ASSERT_NE(updateEntry, nullptr) << error;
    error.clear();
    void* finalizeEntry = finalizeRunner.CompileKernelAst(
        *finalizeProgram, "agg_finalize", &error);
    ASSERT_NE(finalizeEntry, nullptr) << error;

    using TDispatchFn = int64_t(*)(THashTable*, NQqb::TRowSet*, int64_t, int64_t);
    using TFinalizeFn = int64_t(*)(THashTable*, void**, int64_t**, int64_t);
    auto dispatch = reinterpret_cast<TDispatchFn>(updateEntry);
    auto finalize = reinterpret_cast<TFinalizeFn>(finalizeEntry);
    THashTable table;
    ASSERT_NE(dispatch(&table, nullptr, 4, 0), 0);
    EXPECT_EQ(table.KeySize, sizeof(TPairI64Key));

    struct TExpectedState {
        int64_t Sum = 0;
        int64_t Count = 0;
    };
    struct TPairHash {
        size_t operator()(const TPairI64Key& value) const {
            return std::hash<int64_t>{}(value.First) ^
                (std::hash<int64_t>{}(value.Second) << 1);
        }
    };
    std::unordered_map<TPairI64Key, TExpectedState, TPairHash> expected;
    auto runBatch = [&](std::vector<int64_t>& first,
                        std::vector<int64_t>& second,
                        std::vector<int64_t>& values,
                        std::vector<uint8_t>* selection) {
        NQqb::TColumn columns[3] = {};
        columns[0].Data = reinterpret_cast<char*>(first.data());
        columns[1].Data = reinterpret_cast<char*>(second.data());
        columns[2].Data = reinterpret_cast<char*>(values.data());
        NQqb::TRowSet batch{};
        batch.Columns = columns;
        batch.ColumnCount = 3;
        batch.RowCount = static_cast<int64_t>(first.size());
        batch.Selection = selection ? selection->data() : nullptr;
        EXPECT_EQ(dispatch(&table, &batch, 0, 1), 0);
        for (size_t i = 0; i < first.size(); ++i) {
            if (selection && !(*selection)[i]) continue;
            auto& state = expected[{first[i], second[i]}];
            state.Sum += values[i];
            state.Count += 1;
        }
    };

    std::vector<int64_t> first1 = {1, 2, 1, 3, 4, 5};
    std::vector<int64_t> second1 = {10, 20, 10, 30, 40, 50};
    std::vector<int64_t> values1 = {5, 7, 11, 13, 17, 19};
    std::vector<uint8_t> selection1 = {1, 0, 1, 1, 0, 1};
    runBatch(first1, second1, values1, &selection1);

    std::vector<int64_t> first2 = {2, 6, 3, 7, 8, 1};
    std::vector<int64_t> second2 = {20, 60, 30, 70, 80, 11};
    std::vector<int64_t> values2 = {3, 23, -2, 29, 31, 37};
    runBatch(first2, second2, values2, nullptr);

    ASSERT_EQ(table.Size, static_cast<int64_t>(expected.size()));
    ASSERT_GE(table.Capacity, 16);
    auto* groupKeys = reinterpret_cast<const TPairI64Key*>(table.GroupKeys);
    for (int64_t dense = 0; dense < table.Size; ++dense) {
        auto it = expected.find(groupKeys[dense]);
        ASSERT_NE(it, expected.end());
        EXPECT_EQ(table.AggBuffers[0][dense], it->second.Sum);
        EXPECT_EQ(table.AggBuffers[1][dense], it->second.Count);
    }

    std::vector<int64_t> outputFirst(table.Size);
    std::vector<int64_t> outputSecond(table.Size);
    std::vector<int64_t> outputSums(table.Size);
    std::vector<int64_t> outputCounts(table.Size);
    void* outputKeyBuffers[] = {outputFirst.data(), outputSecond.data()};
    int64_t* outputBuffers[] = {outputSums.data(), outputCounts.data()};
    ASSERT_EQ(finalize(
        &table, outputKeyBuffers, outputBuffers, table.Size), table.Size);
    for (int64_t dense = 0; dense < table.Size; ++dense) {
        auto it = expected.find({outputFirst[dense], outputSecond[dense]});
        ASSERT_NE(it, expected.end());
        EXPECT_EQ(outputSums[dense], it->second.Sum);
        EXPECT_EQ(outputCounts[dense], it->second.Count);
    }

    EXPECT_EQ(dispatch(&table, nullptr, 0, 2), 1);
}

TEST(AggregationKernel, GenericKeyEqualityDispatchesToI64Overload) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("generic_key_equal.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TEqualFn = bool(*)(int64_t, int64_t);
    auto equal = reinterpret_cast<TEqualFn>(entry);
    EXPECT_TRUE(equal(0, 0));
    EXPECT_TRUE(equal(-17, -17));
    EXPECT_FALSE(equal(1, -1));
    EXPECT_FALSE(equal(42, 43));
}

TEST(AggregationKernel, GenericHashDispatchesToConcretePairOverload) {
    void* i64Entry = nullptr;
    void* pairEntry = nullptr;
    std::string error;
    auto i64Runner = CompileKernel("generic_dispatch_i64.oz", i64Entry, error);
    ASSERT_NE(i64Entry, nullptr) << error;
    auto pairRunner = CompileKernel("generic_dispatch_pair_i64.oz", pairEntry, error);
    ASSERT_NE(pairEntry, nullptr) << error;

    using TI64DispatchFn = int64_t(*)(int64_t);
    using TPairDispatchFn = int64_t(*)(TPairI64Key);
    auto dispatchI64 = reinterpret_cast<TI64DispatchFn>(i64Entry);
    auto dispatchPair = reinterpret_cast<TPairDispatchFn>(pairEntry);
    EXPECT_EQ(dispatchI64(7), 1007);
    EXPECT_EQ(dispatchPair({7, 13}), 713);
}

TEST(AggregationKernel, GenericPairRobinHoodHandlesCollisionsDuplicatesAndReducers) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("generic_pair_fixed.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TPairBatchFn = int64_t(*)(
        TPairI64Key*, int64_t*, int64_t*, int64_t, int64_t*, TPairI64Key*,
        int64_t*, int64_t*, TPairI64Key*, int64_t*, int64_t);
    auto process = reinterpret_cast<TPairBatchFn>(entry);
    constexpr int64_t capacity = 8;
    std::array<TPairI64Key, capacity> keys{};
    std::array<int64_t, capacity> dist{};
    std::array<int64_t, capacity> slotIds{};
    std::array<TPairI64Key, capacity> groupKeys{};
    std::array<int64_t, capacity> counts{};
    std::array<int64_t, capacity> sums{};
    dist.fill(-1);
    slotIds.fill(-1);
    int64_t size = 0;

    std::array<TPairI64Key, 7> input = {
        TPairI64Key{1, 10}, {2, 20}, {1, 10}, {3, 30}, {4, 40}, {2, 20}, {5, 50}};
    std::array<int64_t, 7> values = {5, 7, 11, 13, 17, -2, 19};
    ASSERT_EQ(process(keys.data(), dist.data(), slotIds.data(), capacity, &size,
                      groupKeys.data(), counts.data(), sums.data(), input.data(),
                      values.data(), input.size()), 5);

    EXPECT_EQ(size, 5);
    EXPECT_EQ(groupKeys[0], (TPairI64Key{1, 10}));
    EXPECT_EQ(groupKeys[1], (TPairI64Key{2, 20}));
    EXPECT_EQ(groupKeys[2], (TPairI64Key{3, 30}));
    EXPECT_EQ(groupKeys[3], (TPairI64Key{4, 40}));
    EXPECT_EQ(groupKeys[4], (TPairI64Key{5, 50}));
    EXPECT_EQ((std::array<int64_t, 5>{counts[0], counts[1], counts[2], counts[3], counts[4]}),
              (std::array<int64_t, 5>{2, 2, 1, 1, 1}));
    EXPECT_EQ((std::array<int64_t, 5>{sums[0], sums[1], sums[2], sums[3], sums[4]}),
              (std::array<int64_t, 5>{16, 5, 13, 17, 19}));

    std::array<bool, capacity> seen{};
    for (int64_t slot = 0; slot < capacity; ++slot) {
        if (dist[slot] >= 0) {
            ASSERT_GE(slotIds[slot], 0);
            ASSERT_LT(slotIds[slot], size);
            EXPECT_FALSE(seen[slotIds[slot]]);
            seen[slotIds[slot]] = true;
        }
    }
    for (int64_t slot = 0; slot < size; ++slot) {
        EXPECT_TRUE(seen[slot]);
    }
}

TEST(AggregationKernel, GenericPairRehashPreservesSlotsAndAggregationState) {
    void* processEntry = nullptr;
    void* rehashEntry = nullptr;
    std::string error;
    auto processRunner = CompileKernel("generic_pair_fixed.oz", processEntry, error);
    ASSERT_NE(processEntry, nullptr) << error;
    auto rehashRunner = CompileKernel("generic_pair_rehash.oz", rehashEntry, error);
    ASSERT_NE(rehashEntry, nullptr) << error;

    using TPairBatchFn = int64_t(*)(
        TPairI64Key*, int64_t*, int64_t*, int64_t, int64_t*, TPairI64Key*,
        int64_t*, int64_t*, TPairI64Key*, int64_t*, int64_t);
    using TPairRehashFn = int64_t(*)(
        TPairI64Key*, int64_t*, int64_t*, int64_t, int64_t, TPairI64Key*,
        int64_t*, int64_t*, TPairI64Key*, int64_t*, int64_t*, int64_t,
        TPairI64Key*, int64_t*, int64_t*);
    auto process = reinterpret_cast<TPairBatchFn>(processEntry);
    auto rehash = reinterpret_cast<TPairRehashFn>(rehashEntry);

    constexpr int64_t oldCapacity = 8;
    constexpr int64_t newCapacity = 16;
    std::array<TPairI64Key, oldCapacity> oldKeys{};
    std::array<int64_t, oldCapacity> oldDist{};
    std::array<int64_t, oldCapacity> oldSlotIds{};
    std::array<TPairI64Key, oldCapacity> oldGroupKeys{};
    std::array<int64_t, oldCapacity> oldCounts{};
    std::array<int64_t, oldCapacity> oldSums{};
    oldDist.fill(-1);
    oldSlotIds.fill(-1);
    int64_t size = 0;
    std::array<TPairI64Key, 5> initialKeys = {
        TPairI64Key{1, 2}, {3, 4}, {5, 6}, {1, 2}, {7, 8}};
    std::array<int64_t, 5> initialValues = {10, 20, 30, 5, 40};
    ASSERT_EQ(process(oldKeys.data(), oldDist.data(), oldSlotIds.data(), oldCapacity,
                      &size, oldGroupKeys.data(), oldCounts.data(), oldSums.data(),
                      initialKeys.data(), initialValues.data(), initialKeys.size()), 4);

    std::array<TPairI64Key, newCapacity> newKeys{};
    std::array<int64_t, newCapacity> newDist{};
    std::array<int64_t, newCapacity> newSlotIds{};
    std::array<TPairI64Key, newCapacity> newGroupKeys{};
    std::array<int64_t, newCapacity> newCounts{};
    std::array<int64_t, newCapacity> newSums{};
    ASSERT_EQ(rehash(oldKeys.data(), oldDist.data(), oldSlotIds.data(), oldCapacity,
                     size, oldGroupKeys.data(), oldCounts.data(), oldSums.data(),
                     newKeys.data(), newDist.data(), newSlotIds.data(), newCapacity,
                     newGroupKeys.data(), newCounts.data(), newSums.data()), size);

    EXPECT_EQ(newGroupKeys[0], (TPairI64Key{1, 2}));
    EXPECT_EQ(newGroupKeys[1], (TPairI64Key{3, 4}));
    EXPECT_EQ(newGroupKeys[2], (TPairI64Key{5, 6}));
    EXPECT_EQ(newGroupKeys[3], (TPairI64Key{7, 8}));
    EXPECT_EQ((std::array<int64_t, 4>{newCounts[0], newCounts[1], newCounts[2], newCounts[3]}),
              (std::array<int64_t, 4>{2, 1, 1, 1}));
    EXPECT_EQ((std::array<int64_t, 4>{newSums[0], newSums[1], newSums[2], newSums[3]}),
              (std::array<int64_t, 4>{15, 20, 30, 40}));

    std::array<TPairI64Key, 3> moreKeys = {
        TPairI64Key{3, 4}, {9, 10}, {11, 12}};
    std::array<int64_t, 3> moreValues = {-7, 50, 60};
    ASSERT_EQ(process(newKeys.data(), newDist.data(), newSlotIds.data(), newCapacity,
                      &size, newGroupKeys.data(), newCounts.data(), newSums.data(),
                      moreKeys.data(), moreValues.data(), moreKeys.size()), 6);
    EXPECT_EQ(newGroupKeys[4], (TPairI64Key{9, 10}));
    EXPECT_EQ(newGroupKeys[5], (TPairI64Key{11, 12}));
    EXPECT_EQ(newCounts[1], 2);
    EXPECT_EQ(newSums[1], 13);
    EXPECT_EQ(newCounts[4], 1);
    EXPECT_EQ(newSums[4], 50);

    std::array<bool, newCapacity> seen{};
    for (int64_t slot = 0; slot < newCapacity; ++slot) {
        if (newDist[slot] >= 0) {
            ASSERT_GE(newSlotIds[slot], 0);
            ASSERT_LT(newSlotIds[slot], size);
            EXPECT_FALSE(seen[newSlotIds[slot]]);
            seen[newSlotIds[slot]] = true;
        }
    }
    for (int64_t slot = 0; slot < size; ++slot) {
        EXPECT_TRUE(seen[slot]);
    }
}

TEST(AggregationKernel, GenericKeyPointerReadWriteAndSwap) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("generic_key_memory.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TMemoryFn = int64_t(*)(int64_t*, int64_t, int64_t, int64_t);
    auto mutate = reinterpret_cast<TMemoryFn>(entry);
    int64_t keys[] = {10, 20, 30};

    EXPECT_EQ(mutate(keys, 0, 2, 99), 30);
    EXPECT_EQ(keys[0], 30);
    EXPECT_EQ(keys[1], 20);
    EXPECT_EQ(keys[2], 99);
}

TEST(AggregationKernel, ConcreteI64PointerWriteControl) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("concrete_key_memory.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TMemoryFn = int64_t(*)(int64_t*, int64_t, int64_t);
    auto write = reinterpret_cast<TMemoryFn>(entry);
    int64_t keys[] = {10, 20, 30};

    EXPECT_EQ(write(keys, 1, 77), 77);
    EXPECT_EQ(keys[0], 10);
    EXPECT_EQ(keys[1], 77);
    EXPECT_EQ(keys[2], 30);
}

TEST(AggregationKernel, I64HashHasStableBoundaryVectors) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("i64_hash.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using THashFn = int64_t(*)(int64_t);
    auto hash = reinterpret_cast<THashFn>(entry);
    EXPECT_EQ(hash(0), 0);
    EXPECT_EQ(hash(1), 5180492295206395165LL);
    EXPECT_EQ(hash(-1), -491796270583644160LL);
    EXPECT_EQ(hash(std::numeric_limits<int64_t>::min()), -1079387622448562176LL);
    EXPECT_EQ(hash(std::numeric_limits<int64_t>::max()), -245898135291822080LL);
}

TEST(AggregationKernel, HomeSlotAndNextIndexUsePowerOfTwoMask) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("slot_math.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TSlotMathFn = int64_t(*)(int64_t, int64_t, int64_t);
    auto slotMath = reinterpret_cast<TSlotMathFn>(entry);
    constexpr std::array<int64_t, 4> capacities = {4, 8, 16, 32};
    constexpr std::array<int64_t, 5> hashes = {
        0, 1, -1, 17, std::numeric_limits<int64_t>::min()};
    for (int64_t capacity : capacities) {
        for (int64_t hash : hashes) {
            const auto home = slotMath(hash, capacity, 0);
            EXPECT_GE(home, 0);
            EXPECT_LT(home, capacity);
            EXPECT_EQ(home, hash & (capacity - 1));
        }
        EXPECT_EQ(slotMath(capacity - 2, capacity, 1), capacity - 1);
        EXPECT_EQ(slotMath(capacity - 1, capacity, 1), 0);
    }
}

TEST(AggregationKernel, ReadOnlyRobinHoodLookup) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("lookup.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TLookupFn = int64_t(*)(int64_t*, int64_t*, int64_t, int64_t);
    auto lookup = reinterpret_cast<TLookupFn>(entry);
    constexpr int64_t capacity = 8;
    std::array<int64_t, capacity> keys{};
    std::array<int64_t, capacity> dist{};
    dist.fill(-1);

    EXPECT_EQ(lookup(keys.data(), dist.data(), capacity, 42), -1);

    const auto home3 = KeysForHome(3, capacity, 3);
    keys[3] = home3[0];
    dist[3] = 0;
    EXPECT_EQ(lookup(keys.data(), dist.data(), capacity, home3[0]), 3);

    keys[4] = home3[1];
    dist[4] = 1;
    keys[5] = home3[2];
    dist[5] = 2;
    EXPECT_EQ(lookup(keys.data(), dist.data(), capacity, home3[2]), 5);

    keys.fill(0);
    dist.fill(-1);
    const auto home7 = KeysForHome(7, capacity, 2);
    keys[7] = home7[0];
    dist[7] = 0;
    keys[0] = home7[1];
    dist[0] = 1;
    EXPECT_EQ(lookup(keys.data(), dist.data(), capacity, home7[1]), 0);

    keys.fill(0);
    dist.fill(-1);
    const auto home2 = KeysForHome(2, capacity, 2);
    const auto home3Single = KeysForHome(3, capacity, 1);
    keys[2] = home2[0];
    dist[2] = 0;
    keys[3] = home3Single[0];
    dist[3] = 0;
    EXPECT_EQ(lookup(keys.data(), dist.data(), capacity, home2[1]), -1);

    for (int64_t i = 0; i < capacity; ++i) {
        keys[i] = i + 1000;
        dist[i] = capacity;
    }
    EXPECT_EQ(lookup(keys.data(), dist.data(), capacity, -999), -1);
}

TEST(AggregationKernel, FixedCapacityRobinHoodInsertion) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("insert_fixed.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TInsertFn = int64_t(*)(int64_t*, int64_t*, int64_t, int64_t);
    auto insert = reinterpret_cast<TInsertFn>(entry);
    constexpr int64_t capacity = 8;
    std::array<int64_t, capacity> keys{};
    std::array<int64_t, capacity> dist{};
    dist.fill(-1);

    const auto home4 = KeysForHome(4, capacity, 3);
    EXPECT_EQ(insert(keys.data(), dist.data(), capacity, home4[0]), 4);
    EXPECT_EQ(keys[4], home4[0]);
    EXPECT_EQ(dist[4], 0);

    const auto beforeKeys = keys;
    const auto beforeDist = dist;
    EXPECT_EQ(insert(keys.data(), dist.data(), capacity, home4[0]), 4);
    EXPECT_EQ(keys, beforeKeys);
    EXPECT_EQ(dist, beforeDist);

    EXPECT_EQ(insert(keys.data(), dist.data(), capacity, home4[1]), 5);
    EXPECT_EQ(keys[5], home4[1]);
    EXPECT_EQ(dist[5], 1);

    keys.fill(0);
    dist.fill(-1);
    const auto home5 = KeysForHome(5, capacity, 1);
    const auto home6 = KeysForHome(6, capacity, 1);
    keys[4] = home4[0];
    dist[4] = 0;
    keys[5] = home5[0];
    dist[5] = 0;
    keys[6] = home6[0];
    dist[6] = 0;
    EXPECT_EQ(insert(keys.data(), dist.data(), capacity, home4[1]), 5);
    EXPECT_EQ(keys[4], home4[0]);
    EXPECT_EQ(keys[5], home4[1]);
    EXPECT_EQ(keys[6], home5[0]);
    EXPECT_EQ(keys[7], home6[0]);
    EXPECT_EQ(dist[4], 0);
    EXPECT_EQ(dist[5], 1);
    EXPECT_EQ(dist[6], 1);
    EXPECT_EQ(dist[7], 1);

    keys.fill(0);
    dist.fill(-1);
    const auto home7 = KeysForHome(7, capacity, 2);
    const auto home0 = KeysForHome(0, capacity, 1);
    keys[7] = home7[0];
    dist[7] = 0;
    keys[0] = home0[0];
    dist[0] = 0;
    EXPECT_EQ(insert(keys.data(), dist.data(), capacity, home7[1]), 0);
    EXPECT_EQ(keys[7], home7[0]);
    EXPECT_EQ(keys[0], home7[1]);
    EXPECT_EQ(keys[1], home0[0]);
    EXPECT_EQ(dist[0], 1);
    EXPECT_EQ(dist[1], 1);

    for (int64_t i = 0; i < capacity; ++i) {
        keys[i] = i;
        dist[i] = capacity;
    }
    EXPECT_EQ(insert(keys.data(), dist.data(), capacity, 123456), -1);
}

TEST(AggregationKernel, FixedCapacityTableMaintainsInvariants) {
    void* insertEntry = nullptr;
    std::string insertError;
    auto insertRunner = CompileKernel("insert_fixed.oz", insertEntry, insertError);
    ASSERT_NE(insertEntry, nullptr) << insertError;

    void* checkEntry = nullptr;
    std::string checkError;
    auto checkRunner = CompileKernel("check_invariants.oz", checkEntry, checkError);
    ASSERT_NE(checkEntry, nullptr) << checkError;

    using TInsertFn = int64_t(*)(int64_t*, int64_t*, int64_t, int64_t);
    using TCheckFn = bool(*)(int64_t*, int64_t*, int64_t);
    auto insert = reinterpret_cast<TInsertFn>(insertEntry);
    auto check = reinterpret_cast<TCheckFn>(checkEntry);
    constexpr int64_t capacity = 16;
    std::array<int64_t, capacity> keys{};
    std::array<int64_t, capacity> dist{};
    dist.fill(-1);

    constexpr std::array<int64_t, 12> input = {
        0, 1, -1, 17, 33, 49, 65, 81, 97, 113, 129, 145};
    EXPECT_TRUE(check(keys.data(), dist.data(), capacity));
    for (int64_t key : input) {
        ASSERT_NE(insert(keys.data(), dist.data(), capacity, key), -1);
        EXPECT_TRUE(check(keys.data(), dist.data(), capacity)) << "after key " << key;
    }

    auto corrupted = dist;
    for (int64_t i = 0; i < capacity; ++i) {
        if (corrupted[i] >= 0) {
            ++corrupted[i];
            break;
        }
    }
    EXPECT_FALSE(check(keys.data(), corrupted.data(), capacity));
}

TEST(AggregationKernel, StableDenseSlotIdsSurviveDisplacement) {
    void* entry = nullptr;
    std::string error;
    auto runner = CompileKernel("insert_slot_id.oz", entry, error);
    ASSERT_NE(entry, nullptr) << error;

    using TInsertFn = int64_t(*)(
        int64_t*, int64_t*, int64_t*, int64_t, int64_t*, int64_t);
    auto insert = reinterpret_cast<TInsertFn>(entry);
    constexpr int64_t capacity = 8;
    std::array<int64_t, capacity> keys{};
    std::array<int64_t, capacity> dist{};
    std::array<int64_t, capacity> slotIds{};
    dist.fill(-1);
    slotIds.fill(-1);
    int64_t size = 0;

    const auto home4 = KeysForHome(4, capacity, 2);
    const auto home5 = KeysForHome(5, capacity, 1);
    const auto home6 = KeysForHome(6, capacity, 1);
    EXPECT_EQ(insert(keys.data(), dist.data(), slotIds.data(), capacity, &size, home4[0]), 0);
    EXPECT_EQ(insert(keys.data(), dist.data(), slotIds.data(), capacity, &size, home5[0]), 1);
    EXPECT_EQ(insert(keys.data(), dist.data(), slotIds.data(), capacity, &size, home6[0]), 2);
    EXPECT_EQ(size, 3);

    EXPECT_EQ(insert(keys.data(), dist.data(), slotIds.data(), capacity, &size, home4[1]), 3);
    EXPECT_EQ(size, 4);
    EXPECT_EQ(keys[4], home4[0]);
    EXPECT_EQ(slotIds[4], 0);
    EXPECT_EQ(keys[5], home4[1]);
    EXPECT_EQ(slotIds[5], 3);
    EXPECT_EQ(keys[6], home5[0]);
    EXPECT_EQ(slotIds[6], 1);
    EXPECT_EQ(keys[7], home6[0]);
    EXPECT_EQ(slotIds[7], 2);

    EXPECT_EQ(insert(keys.data(), dist.data(), slotIds.data(), capacity, &size, home5[0]), 1);
    EXPECT_EQ(size, 4);

    std::array<bool, capacity> seen{};
    for (int64_t i = 0; i < capacity; ++i) {
        if (dist[i] >= 0) {
            ASSERT_GE(slotIds[i], 0);
            ASSERT_LT(slotIds[i], size);
            EXPECT_FALSE(seen[slotIds[i]]);
            seen[slotIds[i]] = true;
        }
    }
    for (int64_t slot = 0; slot < size; ++slot) {
        EXPECT_TRUE(seen[slot]);
    }
}

TEST(AggregationKernel, OzGeneratedReducersAreCalledByStaticName) {
    using TSmokeFn = void(*)(int64_t, int64_t, int64_t, int64_t*);

    // N=4 with the standard count/sum/min/max order.
    {
        void* entry = nullptr;
        std::string error;
        auto runner = CompileReducerSmoke({"count", "sum", "min", "max"}, entry, error);
        ASSERT_NE(entry, nullptr) << error;
        auto smoke = reinterpret_cast<TSmokeFn>(entry);

        std::array<int64_t, 4> out{};
        smoke(/*prev=*/0, /*value=*/10, /*is_new=*/1, out.data());
        EXPECT_EQ(out[0], 1);   // count: first value -> count = 1
        EXPECT_EQ(out[1], 10);  // sum: prev + value
        EXPECT_EQ(out[2], 10);  // min: is_new -> value
        EXPECT_EQ(out[3], 10);  // max: is_new -> value

        smoke(/*prev=*/5, /*value=*/3, /*is_new=*/0, out.data());
        EXPECT_EQ(out[0], 6);   // count: prev + 1
        EXPECT_EQ(out[1], 8);   // sum: prev + value
        EXPECT_EQ(out[2], 3);   // min: value < prev -> value
        EXPECT_EQ(out[3], 5);   // max: value < prev -> prev

        smoke(/*prev=*/5, /*value=*/9, /*is_new=*/0, out.data());
        EXPECT_EQ(out[2], 5);   // min: value > prev -> prev
        EXPECT_EQ(out[3], 9);   // max: value > prev -> value
    }

    // N=2, non-default order/composition (max then count): proves the set
    // and order of generated reducers is exactly `funcs`, not a fixed 4.
    {
        void* entry = nullptr;
        std::string error;
        auto runner = CompileReducerSmoke({"max", "count"}, entry, error);
        ASSERT_NE(entry, nullptr) << error;
        auto smoke = reinterpret_cast<TSmokeFn>(entry);

        std::array<int64_t, 2> out{};
        smoke(/*prev=*/0, /*value=*/-7, /*is_new=*/1, out.data());
        EXPECT_EQ(out[0], -7); // reduce_0 == max, is_new -> value
        EXPECT_EQ(out[1], 1);  // reduce_1 == count

        smoke(/*prev=*/-7, /*value=*/-3, /*is_new=*/0, out.data());
        EXPECT_EQ(out[0], -3); // max(-7, -3) == -3
        EXPECT_EQ(out[1], -6); // count: prev + 1
    }

    // N=1.
    {
        void* entry = nullptr;
        std::string error;
        auto runner = CompileReducerSmoke({"sum"}, entry, error);
        ASSERT_NE(entry, nullptr) << error;
        auto smoke = reinterpret_cast<TSmokeFn>(entry);

        std::array<int64_t, 1> out{};
        smoke(/*prev=*/100, /*value=*/23, /*is_new=*/0, out.data());
        EXPECT_EQ(out[0], 123); // sum: prev + value
    }
}

TEST(AggregationKernel, OzApplyReducersUpdatesAllBuffersByStaticName) {
    using TApplyFn = void(*)(int64_t**, int64_t, int64_t, int64_t);

    // N=4 with the standard count/sum/min/max order.
    {
        void* entry = nullptr;
        std::string error;
        auto runner = CompileApplyReducersSmoke({"count", "sum", "min", "max"}, entry, error);
        ASSERT_NE(entry, nullptr) << error;
        auto apply = reinterpret_cast<TApplyFn>(entry);

        std::array<int64_t, 1> counts{0};
        std::array<int64_t, 1> sums{0};
        std::array<int64_t, 1> mins{0};
        std::array<int64_t, 1> maxs{0};
        std::array<int64_t*, 4> buffers{counts.data(), sums.data(), mins.data(), maxs.data()};

        apply(buffers.data(), /*dense_slot=*/0, /*value=*/10, /*is_new=*/1);
        EXPECT_EQ(counts[0], 1);
        EXPECT_EQ(sums[0], 10);
        EXPECT_EQ(mins[0], 10);
        EXPECT_EQ(maxs[0], 10);

        apply(buffers.data(), /*dense_slot=*/0, /*value=*/3, /*is_new=*/0);
        EXPECT_EQ(counts[0], 2);
        EXPECT_EQ(sums[0], 13);
        EXPECT_EQ(mins[0], 3);
        EXPECT_EQ(maxs[0], 10);

        apply(buffers.data(), /*dense_slot=*/0, /*value=*/20, /*is_new=*/0);
        EXPECT_EQ(counts[0], 3);
        EXPECT_EQ(sums[0], 33);
        EXPECT_EQ(mins[0], 3);
        EXPECT_EQ(maxs[0], 20);
    }

    // N=2, non-default order/composition (max then count), at a non-zero
    // dense_slot to confirm indexing into agg_buffers[i][dense_slot].
    {
        void* entry = nullptr;
        std::string error;
        auto runner = CompileApplyReducersSmoke({"max", "count"}, entry, error);
        ASSERT_NE(entry, nullptr) << error;
        auto apply = reinterpret_cast<TApplyFn>(entry);

        std::array<int64_t, 2> maxs{0, 0};
        std::array<int64_t, 2> counts{0, 0};
        std::array<int64_t*, 2> buffers{maxs.data(), counts.data()};

        apply(buffers.data(), /*dense_slot=*/1, /*value=*/-7, /*is_new=*/1);
        EXPECT_EQ(maxs[1], -7);  // reduce_0 == max, is_new -> value
        EXPECT_EQ(counts[1], 1); // reduce_1 == count
        EXPECT_EQ(maxs[0], 0);   // slot 0 untouched
        EXPECT_EQ(counts[0], 0); // slot 0 untouched

        apply(buffers.data(), /*dense_slot=*/1, /*value=*/-3, /*is_new=*/0);
        EXPECT_EQ(maxs[1], -3);  // max(-7, -3) == -3
        EXPECT_EQ(counts[1], 2); // count: prev + 1
    }

    // N=1.
    {
        void* entry = nullptr;
        std::string error;
        auto runner = CompileApplyReducersSmoke({"sum"}, entry, error);
        ASSERT_NE(entry, nullptr) << error;
        auto apply = reinterpret_cast<TApplyFn>(entry);

        std::array<int64_t, 1> sums{100};
        std::array<int64_t*, 1> buffers{sums.data()};

        apply(buffers.data(), /*dense_slot=*/0, /*value=*/23, /*is_new=*/0);
        EXPECT_EQ(sums[0], 123); // sum: prev + value
    }
}

// L3: TKernelCompiler::CompileAggregate over a synthetic TStructType, exercising
// Dispatch (agg_dispatch, built from L2a+L2b+L2c generators) and Finalize
// (agg_finalize, L2b-2) end to end with no planner/exec involved.
TEST(AggregationCompiler, CompileAggregateDispatchAndFinalize) {
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    TStructType inputType(std::vector<std::pair<std::string, TTypePtr>>{
        {"k", i64Type},
        {"v", i64Type},
    });

    std::vector<NQqb::TAggregateSpec> aggs = {
        {"c", "count", nullptr},
        {"s", "sum", std::make_shared<TIdentExpr>(loc, "v")},
        {"mn", "min", std::make_shared<TIdentExpr>(loc, "v")},
        {"mx", "max", std::make_shared<TIdentExpr>(loc, "v")},
    };

    NQqb::TKernelCompiler compiler;
    auto kernels = compiler.CompileAggregate(inputType, {"k"}, aggs);
    EXPECT_EQ(kernels.NumAggs, 4u);

    std::array<uint8_t, NQqb::TKernelCompiler::kHashTableSize> htBuf{};
    void* ht = htBuf.data();

    ASSERT_TRUE(kernels.Dispatch(ht, nullptr, /*capacity=*/4, /*op=init*/0));

    std::vector<int64_t> keys = {1, 2, 1, 3, 2, 1};
    std::vector<int64_t> vals = {10, 20, 5, 7, -3, 9};
    NQqb::TColumn cols[2] = {};
    cols[0].Data = reinterpret_cast<char*>(keys.data());
    cols[1].Data = reinterpret_cast<char*>(vals.data());
    NQqb::TRowSet rowSet{};
    rowSet.Columns = cols;
    rowSet.ColumnCount = 2;
    rowSet.RowCount = static_cast<int64_t>(keys.size());
    rowSet.Selection = nullptr;

    EXPECT_EQ(kernels.Dispatch(ht, &rowSet, 0, /*op=update*/1), 0);

    auto* table = reinterpret_cast<THashTable*>(ht);
    ASSERT_EQ(table->Size, 3);

    std::array<int64_t, 3> outputKeys{};
    std::array<int64_t, 3> outputCounts{};
    std::array<int64_t, 3> outputSums{};
    std::array<int64_t, 3> outputMins{};
    std::array<int64_t, 3> outputMaxs{};
    std::array<int64_t*, 4> outputBuffers{
        outputCounts.data(), outputSums.data(), outputMins.data(), outputMaxs.data()};

    std::array<void*, 1> outputKeyBuffers{outputKeys.data()};
    EXPECT_EQ(kernels.Finalize(ht, outputKeyBuffers.data(), outputBuffers.data(), 3), 3);

    struct TGroupStats {
        int64_t Count = 0;
        int64_t Sum = 0;
        int64_t Min = 0;
        int64_t Max = 0;
        bool Seen = false;
    };
    std::unordered_map<int64_t, TGroupStats> reference;
    for (size_t i = 0; i < keys.size(); ++i) {
        auto& group = reference[keys[i]];
        if (!group.Seen) {
            group.Min = vals[i];
            group.Max = vals[i];
            group.Seen = true;
        } else {
            group.Min = std::min(group.Min, vals[i]);
            group.Max = std::max(group.Max, vals[i]);
        }
        group.Count += 1;
        group.Sum += vals[i];
    }

    for (size_t i = 0; i < outputKeys.size(); ++i) {
        auto it = reference.find(outputKeys[i]);
        ASSERT_NE(it, reference.end()) << "unexpected key " << outputKeys[i];
        EXPECT_EQ(outputCounts[i], it->second.Count) << "key " << outputKeys[i];
        EXPECT_EQ(outputSums[i], it->second.Sum) << "key " << outputKeys[i];
        EXPECT_EQ(outputMins[i], it->second.Min) << "key " << outputKeys[i];
        EXPECT_EQ(outputMaxs[i], it->second.Max) << "key " << outputKeys[i];
    }

    EXPECT_TRUE(kernels.Dispatch(ht, nullptr, 0, /*op=destroy*/6));
}

// L4: TRuntimeAggregate over CompileAggregate's kernels, with no planner/sexp
// involved. "Scenario K"-style input: 24 distinct keys in [-12, 11] over 200
// rows split into 3 batches force the HashTable through several grows
// (capacity 4 -> 8 -> 16 -> 32) with collisions and duplicate keys across
// batches; result is compared against a std::unordered_map reference.
TEST(AggregationExec, RuntimeAggregateMultiBatchGrowAndCollisions) {
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    auto i64Type = std::make_shared<TIntegerType>();
    auto structType = std::make_shared<TStructType>(std::vector<std::pair<std::string, TTypePtr>>{
        {"k", i64Type},
        {"v", i64Type},
    });
    TTypePtr inputSchema = structType;

    std::vector<NQqb::TAggregateSpec> aggs = {
        {"c", "count", nullptr},
        {"s", "sum", std::make_shared<TIdentExpr>(loc, "v")},
        {"mn", "min", std::make_shared<TIdentExpr>(loc, "v")},
        {"mx", "max", std::make_shared<TIdentExpr>(loc, "v")},
    };

    NQqb::TKernelCompiler compiler;
    auto kernels = compiler.CompileAggregate(*structType, {"k"}, aggs);
    auto outputType = NQqb::ComputeAggregateOutputType(inputSchema, {"k"}, aggs);

    constexpr size_t rowCount = 200;
    std::vector<int64_t> keys(rowCount);
    std::vector<int64_t> values(rowCount);

    struct TGroupStats {
        int64_t Count = 0;
        int64_t Sum = 0;
        int64_t Min = 0;
        int64_t Max = 0;
        bool Seen = false;
    };
    std::unordered_map<int64_t, TGroupStats> reference;

    uint64_t state = 88172645463325252ULL;
    auto next = [&state]() -> uint64_t {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state;
    };
    for (size_t i = 0; i < rowCount; ++i) {
        const int64_t key = static_cast<int64_t>(next() % 24) - 12;
        const int64_t value = static_cast<int64_t>(next() % 401) - 200;
        keys[i] = key;
        values[i] = value;
        auto& group = reference[key];
        if (!group.Seen) {
            group.Min = value;
            group.Max = value;
            group.Seen = true;
        } else {
            group.Min = std::min(group.Min, value);
            group.Max = std::max(group.Max, value);
        }
        group.Count += 1;
        group.Sum += value;
    }

    // Split into 3 batches of uneven size.
    constexpr size_t batchCount = 3;
    std::vector<std::vector<NQqb::TColumn>> batchColumns(batchCount);
    std::vector<NQqb::TRowSet> batches;
    size_t offset = 0;
    for (size_t batch = 0; batch < batchCount; ++batch) {
        const size_t remaining = rowCount - offset;
        const size_t batchSize =
            (batch + 1 == batchCount) ? remaining : remaining / (batchCount - batch);

        batchColumns[batch] = {
            NQqb::TColumn{.Data = reinterpret_cast<char*>(keys.data() + offset)},
            NQqb::TColumn{.Data = reinterpret_cast<char*>(values.data() + offset)},
        };
        batches.push_back(NQqb::TRowSet{
            .Columns = batchColumns[batch].data(),
            .ColumnCount = 2,
            .RowCount = static_cast<int64_t>(batchSize),
            .Selection = nullptr,
            .Destroy = nullptr,
            .Private = nullptr,
            .RefCount = 1,
        });
        offset += batchSize;
    }
    ASSERT_EQ(offset, rowCount);

    class TVectorRuntimeSource : public NQqb::IRuntimeNode {
    public:
        TVectorRuntimeSource(NQumir::NAst::TTypePtr outputType, std::vector<NQqb::TRowSet> batches)
            : OutputType_(std::move(outputType))
            , Batches_(std::move(batches))
        {}

        NQumir::NAst::TTypePtr OutputType() const override { return OutputType_; }

        bool Next(NQqb::TRowSet& rowSet) override {
            if (Index_ >= Batches_.size()) {
                return false;
            }
            rowSet = Batches_[Index_++];
            return true;
        }

    private:
        NQumir::NAst::TTypePtr OutputType_;
        std::vector<NQqb::TRowSet> Batches_;
        size_t Index_ = 0;
    };

    auto input = std::make_unique<TVectorRuntimeSource>(inputSchema, std::move(batches));
    NQqb::TRuntimeAggregate agg(std::move(input), outputType, std::move(kernels));

    NQqb::TRowSet result{};
    ASSERT_TRUE(agg.Next(result));
    ASSERT_EQ(result.ColumnCount, static_cast<int64_t>(1 + aggs.size()));
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outMins = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* outMaxs = reinterpret_cast<int64_t*>(result.Columns[4].Data);

    std::unordered_set<int64_t> seenKeys;
    for (int64_t i = 0; i < result.RowCount; ++i) {
        const int64_t key = outKeys[i];
        auto it = reference.find(key);
        ASSERT_NE(it, reference.end()) << "unexpected key " << key;
        EXPECT_TRUE(seenKeys.insert(key).second) << "duplicate key " << key;
        EXPECT_EQ(outCounts[i], it->second.Count) << "key " << key;
        EXPECT_EQ(outSums[i], it->second.Sum) << "key " << key;
        EXPECT_EQ(outMins[i], it->second.Min) << "key " << key;
        EXPECT_EQ(outMaxs[i], it->second.Max) << "key " << key;
    }
    EXPECT_EQ(seenKeys.size(), reference.size());

    NQqb::Release(&result);

    NQqb::TRowSet second{};
    EXPECT_FALSE(agg.Next(second));
}

// L5: TPhysicalPlanner::Build wires TAggregateOperator (after AnnotateTypes +
// ApplyColumnPruning) onto TRuntimeSource + TRuntimeAggregate, over an
// in-memory ISource with multiple batches.
TEST(AggregationExec, PlannerBuildsAggregateOverSource) {
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    std::vector<int64_t> keys = {1, 2, 1, 3, 2, 1, 4, 4};
    std::vector<int64_t> vals = {10, 20, 5, 7, -3, 9, 100, -100};

    class TVectorSource : public NQqb::ISource {
    public:
        TVectorSource(std::vector<std::string> names, std::vector<NQqb::TRowSet> batches)
            : Names_(std::move(names))
            , Batches_(std::move(batches))
        {
            for (auto& name : Names_) {
                Cols_.push_back({name, std::make_shared<TIntegerType>()});
            }
            Schema_ = NQqb::TSchema{Cols_};
        }

        const NQqb::TSchema& Schema() const override { return Schema_; }

        bool Next(NQqb::TRowSet& rowSet) override {
            if (Index_ >= Batches_.size()) {
                return false;
            }
            rowSet = Batches_[Index_++];
            return true;
        }

    private:
        std::vector<std::string> Names_;
        std::vector<NQqb::TColumnSchema> Cols_;
        NQqb::TSchema Schema_;
        std::vector<NQqb::TRowSet> Batches_;
        size_t Index_ = 0;
    };

    // 2 batches of 4 rows each; key=4 appears only in the second batch.
    constexpr size_t batchSize = 4;
    std::vector<std::vector<NQqb::TColumn>> batchColumns(2);
    std::vector<NQqb::TRowSet> batches;
    for (size_t b = 0; b < 2; ++b) {
        batchColumns[b] = {
            NQqb::TColumn{.Data = reinterpret_cast<char*>(keys.data() + b * batchSize)},
            NQqb::TColumn{.Data = reinterpret_cast<char*>(vals.data() + b * batchSize)},
        };
        batches.push_back(NQqb::TRowSet{
            .Columns = batchColumns[b].data(),
            .ColumnCount = 2,
            .RowCount = static_cast<int64_t>(batchSize),
            .Selection = nullptr,
            .Destroy = nullptr,
            .Private = nullptr,
            .RefCount = 1,
        });
    }

    TVectorSource source({"k", "v"}, std::move(batches));
    auto sourceOp = std::make_shared<NQqb::TSourceOperator>(source);

    std::vector<NQqb::TAggregateSpec> aggs = {
        {"c", "count", nullptr},
        {"s", "sum", std::make_shared<TIdentExpr>(loc, "v")},
        {"mn", "min", std::make_shared<TIdentExpr>(loc, "v")},
        {"mx", "max", std::make_shared<TIdentExpr>(loc, "v")},
    };
    auto aggOp = std::make_shared<NQqb::TAggregateOperator>(sourceOp, std::vector<std::string>{"k"}, aggs);

    NQqb::AnnotateTypes(aggOp);
    NQqb::ApplyColumnPruning(aggOp);

    NQqb::TPhysicalPlanner planner;
    auto runtime = planner.Build(aggOp);

    struct TGroupStats {
        int64_t Count = 0;
        int64_t Sum = 0;
        int64_t Min = 0;
        int64_t Max = 0;
        bool Seen = false;
    };
    std::unordered_map<int64_t, TGroupStats> reference;
    for (size_t i = 0; i < keys.size(); ++i) {
        auto& group = reference[keys[i]];
        if (!group.Seen) {
            group.Min = vals[i];
            group.Max = vals[i];
            group.Seen = true;
        } else {
            group.Min = std::min(group.Min, vals[i]);
            group.Max = std::max(group.Max, vals[i]);
        }
        group.Count += 1;
        group.Sum += vals[i];
    }

    NQqb::TRowSet result{};
    ASSERT_TRUE(runtime->Next(result));
    ASSERT_EQ(result.ColumnCount, static_cast<int64_t>(1 + aggs.size()));
    ASSERT_EQ(result.RowCount, static_cast<int64_t>(reference.size()));

    auto* outKeys = reinterpret_cast<int64_t*>(result.Columns[0].Data);
    auto* outCounts = reinterpret_cast<int64_t*>(result.Columns[1].Data);
    auto* outSums = reinterpret_cast<int64_t*>(result.Columns[2].Data);
    auto* outMins = reinterpret_cast<int64_t*>(result.Columns[3].Data);
    auto* outMaxs = reinterpret_cast<int64_t*>(result.Columns[4].Data);

    for (int64_t i = 0; i < result.RowCount; ++i) {
        const int64_t key = outKeys[i];
        auto it = reference.find(key);
        ASSERT_NE(it, reference.end()) << "unexpected key " << key;
        EXPECT_EQ(outCounts[i], it->second.Count) << "key " << key;
        EXPECT_EQ(outSums[i], it->second.Sum) << "key " << key;
        EXPECT_EQ(outMins[i], it->second.Min) << "key " << key;
        EXPECT_EQ(outMaxs[i], it->second.Max) << "key " << key;
    }

    NQqb::Release(&result);

    NQqb::TRowSet second{};
    EXPECT_FALSE(runtime->Next(second));
}

int main(int argc, char** argv) {
    if (argc > 1) {
        KernelDir = argv[1];
    }
    ::testing::InitGoogleTest(&argc, argv);
    NQumir::NCodeGen::TLLVMInitializer llvmInit;
    return RUN_ALL_TESTS();
}
