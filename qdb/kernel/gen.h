#pragma once

#include <qdb/kernel/aggregate_key.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace NQqb {
namespace NKernel {

// Per-reducer layout for the generic aggregation kernel. One TAggReducerInfo
// per requested aggregate (same order/size as `funcs`/`aggs`); ValidBufIdx
// (if >= 0) names an extra internal AggBuffers slot ("valid count") used to
// skip NULL reducer arguments and to mark NULL output for sum/min/max when a
// group has zero non-null contributions. NumAggBuffers is the total number of
// AggBuffers slots the hash table must allocate (>= Reducers.size()); only
// the first Reducers.size() slots are exposed to TAggregateFinalize's
// outputBuffers.
struct TAggReducerInfo {
    std::string Func;
    bool HasArg = false;            // agg.Arg != nullptr
    bool NeedsValidity = false;     // argIsNullable && HasArg
    bool IsNullableOutput = false;  // NeedsValidity && Func in {sum,min,max}
    int ValidBufIdx = -1;           // -1 if IsNullableOutput == false
};

struct TAggReducerLayout {
    std::vector<TAggReducerInfo> Reducers;
    size_t NumAggBuffers = 0;
};

// Computes the AggBuffers layout for `funcs`/`hasArg` given whether the
// shared reducer argument column is nullable. When argIsNullable is false,
// NumAggBuffers == funcs.size() and every TAggReducerInfo::NeedsValidity is
// false, preserving the existing one-buffer-per-reducer layout exactly.
TAggReducerLayout BuildAggReducerLayout(
    const std::vector<std::string>& funcs,
    const std::vector<bool>& hasArg,
    bool argIsNullable);

// Generates concrete hash and equality overloads for lookup and stored keys.
std::vector<NQumir::NAst::TExprPtr> GenKeyOperationFunDecls(
    const TAggregateKeyDescriptor& key);

// Generates key_owned_bytes(LookupKey) and key_clone_owned(LookupKey, u8*).
std::vector<NQumir::NAst::TExprPtr> GenKeyOwnershipFunDecls(
    const TAggregateKeyDescriptor& key);

void SubstFieldsInPlace(
    NQumir::NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& fieldNames,
    const NQumir::NAst::TExprPtr& indexIdent);

// Builds a vectorized filter kernel that takes (ref TRowSet) directly.
// fieldIndices maps each field name to its index in TRowSet.Columns.
NQumir::NAst::TExprPtr GenFilterKernelAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType,
    std::vector<std::shared_ptr<std::string>>& literalStorage);

// Builds the generic aggregation dispatch entry over aht_init/aht_upsert_dual/
// aht_destroy. Key extraction uses the common TColumn materializer and the
// descriptor's borrowed LookupType/stored StoredType representations. Aggregate
// values still use one shared integer input column and i64 reducer states.
NQumir::NAst::TExprPtr GenGenericAggregateDispatchAst(
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const std::optional<std::string>& argField,
    const TAggReducerLayout& layout,
    bool argIsNullable,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType);

// Builds the byte-oriented finalize ABI. C++ supplies one opaque u8 destination
// per logical key field; the generated wrapper casts each destination to its
// concrete pointer type and projects dense AoS Key values into SoA columns.
NQumir::NAst::TExprPtr GenGenericAggregateFinalizeAst(
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr columnType = nullptr);

// Measures the output Data bytes required for each logical key column.
// Fixed-width columns report row_count * field.Size; string columns report
// the sum of stored byte lengths. Returns ht.Size or -1 on capacity failure.
NQumir::NAst::TExprPtr GenGenericAggregateMeasureAst(
    const TAggregateKeyDescriptor& key,
    NQumir::NAst::TTypePtr hashTableType);

// Generates N = funcs.size() FunDecls named reduce_0..reduce_{N-1}, each with
// the predefined reducer contract from PLAN_AGGREGATION.md section 4:
//   reduce_i(i64 prev, i64 value, bool is_new) -> i64
// funcs[i] selects the body ("count"|"sum"|"min"|"max"), generated under the
// positional name reduce_i. N is exactly funcs.size(), not a fixed builtin set.
//
// The result is meant for static injection via MergeKernelLibrary before
// kernel compilation: generated table code calls reduce_0..reduce_{N-1}
// directly by name (no function pointers, no runtime dispatch on Func).
std::vector<NQumir::NAst::TExprPtr> GenReducerFunDecls(
    const TAggReducerLayout& layout);

// Generates a single FunDecl:
//   agg_apply_reducers(<ptr <ptr i64>> agg_buffers, i64 dense_slot,
//                       i64 value, bool is_new)
// which for i in [0, numReducers) does
//   agg_buffers[i][dense_slot] = reduce_i(agg_buffers[i][dense_slot], value, is_new)
// — numReducers static, direct, by-name calls to the reduce_0..reduce_{N-1}
// functions from GenReducerFunDecls.
//
// This is the one piece of per-query generated code that the NumAggs-generic
// table library (agg_init/agg_rehash/agg_update/agg_destroy/agg_finalize,
// L2b) calls by static name to update all N aggregate buffers for one dense
// slot. Everything else in that library is agg-count-agnostic and driven by
// ht.NumAggs/ht.AggBuffers at runtime.
NQumir::NAst::TExprPtr GenApplyReducersFunDecl(const TAggReducerLayout& layout);

} // namespace NKernel
} // namespace NQqb
