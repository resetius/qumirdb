#pragma once

#include <qdb/io/io.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/join.h>

#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace NQdb {

// agg_dispatch(ref HashTable ht, ref TRowSet batch, i64 arg, i64 op) -> i64
//   op == 0: init(ht, capacity = arg)
//   op == 1: update ht from *batch (arg ignored)
//   otherwise: destroy(ht)
// `ht` must point to a caller-owned, zero-initialized buffer of
// TKernelCompiler::kHashTableSize bytes (the HashTable layout from
// modules/qumirdb.cpp).
using TAggregateDispatch = std::function<int64_t(void* ht, TRowSet* batch, int64_t arg, int64_t op)>;

// Reports required Data bytes for each output key column. Returns ht.Size or
// -1 if outputCapacity is smaller than the number of groups.
using TAggregateMeasure = std::function<int64_t(
    void* ht, int64_t* outputKeyBytes, int64_t outputCapacity)>;

// agg_finalize(ref HashTable ht, <ptr <ptr u8>> outputKeyBuffers, <ptr <ptr i64>>
// outputBuffers, i64 outputCapacity) -> i64. outputBuffers must have NumAggs
// entries, each pointing to an int64_t[outputCapacity] buffer, in the same
// order as the `aggs` passed to CompileAggregate. Returns ht.Size (number of
// groups), or -1 if outputCapacity < ht.Size. outputKeyBuffers has one
// TColumn* destination per group key; generated code fills Data and Mask.
// outputAggMasks has one u8* per aggregate (NumAggs entries, same order as
// `aggs`): a non-null entry points to a bitmap the generated code fills for a
// nullable aggregate output; entries for non-nullable aggregates are ignored.
using TAggregateFinalize = std::function<int64_t(void* ht, void** outputKeyBuffers, int64_t** outputBuffers, uint8_t** outputAggMasks, int64_t outputCapacity)>;

enum class EAggregateOutputKeyKind {
    Fixed,
    String,
};

struct TAggregateOutputKey {
    EAggregateOutputKeyKind Kind = EAggregateOutputKeyKind::Fixed;
    bool IsNullable = false;
    size_t Size = 0;
    size_t Alignment = 0;
};

struct TAggregateOutputAgg {
    bool IsNullable = false;
};

// The compiled kernels for one Aggregation query: Dispatch handles
// init/update/destroy of the HashTable (via agg_dispatch's op-codes),
// Finalize copies the dense group keys/aggregate buffers to output arrays.
struct TAggregateKernels {
    TAggregateDispatch Dispatch;
    TAggregateMeasure Measure;
    TAggregateFinalize Finalize;
    size_t NumAggs = 0;
    std::vector<TAggregateOutputKey> OutputKeys;
    std::vector<TAggregateOutputAgg> OutputAggs;
};

// The compiled kernels for one symmetric hash join. Each side's hash map is a
// caller-owned, zero-initialized HashTable buffer (kHashTableSize); the output
// pair buffer is a zero-initialized buffer of kPairBufferSize. The key columns
// are baked into the generated ProcessLeft / ProcessRight functions.
struct TJoinKernels {
    // jt_init(table, capacity) — initializes one side's HashTable (key size is
    // captured). Returns false on allocation failure.
    std::function<bool(void* table, int64_t capacity)> Init;
    // ProcessLeft(own=left, opp=right, batch, batchIdx, pairs, leftStore,
    // rightStore): reads the left key columns, probes the right table, appends
    // matching pairs (each gated by the injected jt_residual_filter, which reads
    // columns from leftStore/rightStore — contiguous TRowSet arrays — by decoding
    // the packed row IDs), then inserts the batch's rows into the left table.
    // ProcessRight is the mirror. leftStore/rightStore are only dereferenced by a
    // non-trivial residual filter; for the default always-true filter they are
    // optimized away.
    std::function<bool(void* own, void* opp, TRowSet* batch, int64_t batchIdx,
        void* pairs, TRowSet* leftStore, TRowSet* rightStore)> ProcessLeft;
    std::function<bool(void* own, void* opp, TRowSet* batch, int64_t batchIdx,
        void* pairs, TRowSet* leftStore, TRowSet* rightStore)> ProcessRight;
    // Probe-only streaming variants used after one input side reaches EOF.
    // ProbeLeftStream(build=right, batch=left) and ProbeRightStream(build=left,
    // batch=right) emit pairs but never insert stream rows into a hash table.
    std::function<bool(void* build, TRowSet* batch, int64_t batchIdx,
        void* pairs, TRowSet* leftStore, TRowSet* rightStore)> ProbeLeftStream;
    std::function<bool(void* build, TRowSet* batch, int64_t batchIdx,
        void* pairs, TRowSet* leftStore, TRowSet* rightStore)> ProbeRightStream;
    std::function<void(void* table)> DestroyTable;
    std::function<void(void* pairs)> DestroyPairs;

    // Filled only for LeftSemi / LeftAnti joins (null otherwise).
    // InsertKeyOnly(own, /*opp=*/nullptr, batch, /*batchIdx=*/0, /*pairs=*/nullptr):
    //   inserts only the key for each selected row, with no row-ID storage.
    //   Used to build the right-side table for SEMI/ANTI without jb_append.
    // FinalizeAntiSemi(own=left, opp=right, pairs):
    //   iterates own.GroupKeys, probes opp, and pushes matching (SEMI) or
    //   non-matching (ANTI) left RowIds into pairs (right_id = -1).
    std::function<bool(void* own, void* opp, TRowSet* batch, int64_t batchIdx, void* pairs)> InsertKeyOnly;
    std::function<bool(void* own, void* opp, void* pairs)> FinalizeAntiSemi;

    // Filled only for Left / Right outer joins (null otherwise).
    // FinalizeOuter(own, opp, pairs):
    //   iterates own.GroupKeys; for each slot where rh_lookup_slot(opp, key)==-1
    //   (unmatched), drains own RowBucket and pushes (ownRowId, -1) into pairs.
    //   For Left outer: call with own=LeftTable_, opp=RightTable_.
    //   For Right outer: call with own=RightTable_, opp=LeftTable_,
    //   then swap pair halves before draining.
    std::function<bool(void* own, void* opp, void* pairs)> FinalizeOuter;
};

// Compiles qumir core-lang kernel sources to LLVM JIT function pointers.
class TKernelCompiler {
public:
    explicit TKernelCompiler(std::ostream* diagnostics = nullptr)
        : Diagnostics_(diagnostics)
    {
        Opts_.CoreInput = true;
        Opts_.ResolveCoreInput = true;
        Opts_.NativeCode = true;
        Opts_.OptLevel = 3;
        Opts_.AllowOverloads = true;
        Opts_.PrintIr = Diagnostics_ != nullptr;
        Opts_.PrintLlvm = Diagnostics_ != nullptr;
    }

    // Filter dispatch: called per batch to fill the selection buffer.
    using TFilterDispatch = std::function<void(TRowSet& rowSet)>;

    // Project dispatch: called per batch; for each computed column k and row i,
    // writes outBuffers[k][i] = <expr_k>. outBuffers has one pointer per computed
    // projection, each to a caller-owned buffer of RowCount * sizeof(computed
    // type). Only computed (non-ident) columns go through this kernel.
    using TProjectDispatch = std::function<void(TRowSet* in, void** outBuffers)>;

    // Radix sort dispatch over a contiguous key array. `values` points to an
    // array whose concrete element type is selected by CompileRadixSortIndices.
    // `indices` is sorted in place, preserving equal-key order.
    using TSortRadixDispatch = std::function<void(
        void* values, uint32_t* indices, uint32_t* work, uint32_t* counts,
        int64_t n, bool desc)>;

    using TSortRadixCompositeDispatch = std::function<void(
        void** values, uint32_t* indices, uint32_t* work, uint32_t* counts,
        int64_t n, bool* descs)>;

    // sizeof(HashTable) per modules/qumirdb.cpp's layout — callers of
    // CompileAggregate must allocate a zero-initialized buffer this large
    // for `ht`.
    static constexpr size_t kHashTableSize = 104;

    // The symmetric hash join reuses the aggregation HashTable (kHashTableSize)
    // as each side's hash map. sizeof(PairBuffer) per modules/qumirdb.cpp —
    // callers of CompileJoin allocate a zero-initialized buffer this large for
    // the output pair buffer.
    static constexpr size_t kPairBufferSize = 24;

    // Compiles a filter kernel for the given input struct type and predicate.
    // Returns a dispatch lambda that calls the compiled kernel.
    TFilterDispatch CompileFilter(
        const NQumir::NAst::TStructType& inputType,
        const NQumir::NAst::TExprPtr& predicate);

    // Compiles a project kernel for the computed (non-ident) projections.
    // computedExprs and computedTypes are parallel; computedTypes[k] is the
    // inferred output type of computedExprs[k] (see InferProjectExprType). The
    // kernel writes each expression to its output buffer, casting to that type.
    TProjectDispatch CompileProject(
        const NQumir::NAst::TStructType& inputType,
        const std::vector<NQumir::NAst::TExprPtr>& computedExprs,
        const std::vector<NQumir::NAst::TTypePtr>& computedTypes);

    TSortRadixDispatch CompileRadixSortIndices(
        const NQumir::NAst::TTypePtr& type);

    TSortRadixCompositeDispatch CompileRadixSortComposite(
        const std::vector<NQumir::NAst::TTypePtr>& types);

    // Compiles the per-query generic update and finalize programs for `aggs`
    // grouped by `groupKeys`, over rows of `inputType`.
    //
    // Current aggregate-value constraints (NQumir::TError if violated):
    // - one or more integer/f64/string group keys;
    // - agg.Func in {count, sum, min, max};
    // - every agg with Arg references the same single integer column;
    // - all reducer states are i64. count(*) aggs (Arg == nullptr) pass 0.
    TAggregateKernels CompileAggregate(
        const NQumir::NAst::TStructType& inputType,
        const std::vector<std::string>& groupKeys,
        const std::vector<TAggregateSpec>& aggs);

    // Compiles the symmetric hash join kernels for the equi-key pairs `keys`
    // (parallel left/right column names) over rows of leftType/rightType.
    // Constraints (NQumir::TError if violated): type == Inner; key components
    // are fixed-width (integer/f64/composite) and type-compatible across sides.
    // String keys are not supported yet.
    //
    // Optional residual filter: when `residualPredicate` is set, it is compiled
    // into the injected `jt_residual_filter` (evaluated per matched pair before
    // emit), reading columns from the inner schema `innerType` (left fields ++
    // right fields, split at `leftFieldCount`). For LeftSemi/LeftAnti with a
    // residual predicate the process functions are generated as INNER (they emit
    // pairs); the executor dedups matched left IDs.
    TJoinKernels CompileJoin(
        const NQumir::NAst::TStructType& leftType,
        const NQumir::NAst::TStructType& rightType,
        const std::vector<std::pair<std::string, std::string>>& keys,
        EJoinType type,
        const NQumir::NAst::TExprPtr& residualPredicate = nullptr,
        const NQumir::NAst::TStructType* innerType = nullptr,
        size_t leftFieldCount = 0);

private:
    std::ostream* Diagnostics_ = nullptr;
    NQumir::TLLVMRunnerOptions Opts_;
};

} // namespace NQdb
