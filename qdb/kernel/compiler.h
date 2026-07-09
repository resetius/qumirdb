#pragma once

#include <qdb/io/io.h>
#include <qdb/kernel/generated.h>
#include <qdb/kernel/spec.h>
#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/join.h>

#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NQdb {

// Kernel generation appends TGeneratedKernels to Sink (when set); BindNow
// additionally JIT-finalizes them immediately so the returned dispatches are
// live (the default; deferred callers finalize via JitFinalizeKernels).
struct TKernelCompilerOptions {
    std::ostream* Diagnostics = nullptr;
    std::string Stage;
    const void* Operator = nullptr;
    std::vector<TGeneratedKernel>* Sink = nullptr;
    bool BindNow = true;
};

// Shared kernel-pipeline options; caller overrides NativeCode/TargetTriple.
NQumir::TLLVMRunnerOptions KernelRunnerOptions();

// Native-JIT kernel compilation (prepends `use qumirdb`, links runtime
// symbols); one function pointer per entry. The pointers live as long as
// `runner`.
std::unordered_map<std::string, void*> CompileKernelAst(
    NQumir::TLLVMRunner& runner,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    std::string* error);

// Like the native CompileKernelAst, but emits a target object (per
// runner.Options.TargetTriple) instead of JIT. Returns object bytes.
std::optional<std::string> CompileKernelAstToObject(
    NQumir::TLLVMRunner& runner,
    NQumir::NAst::TExprPtr ast,
    const std::vector<std::string>& entryNames,
    std::string* error);

struct TSortRadixKeyInput {
    int32_t ColumnIndex = 0;
    NQumir::NAst::TTypePtr Type;
    bool Desc = false;
    bool NullsFirst = false;
};

// Radix composite sort program (entry qdb_radix_sort_indices_composite): sorts
// packed TRowId values in-place and reads key columns directly from TRowSet
// storage. Throws on an unsupported key type.
NQumir::NAst::TExprPtr BuildRadixSortProgramAst(
    const std::vector<TSortRadixKeyInput>& keys);

// Nullable variant (entry qdb_radix_sort_indices_composite_nullable).
NQumir::NAst::TExprPtr BuildRadixSortNullableProgramAst(
    const std::vector<TSortRadixKeyInput>& keys);

// Top-sort program (entry qdb_top_sort_update): sorts incoming batch row ids
// with the same radix cascade as full sort, then merges the current top-K state
// with that sorted batch. It writes pick_src/pick_idx pairs:
// src=0 -> old state row, src=1 -> incoming batch row.
NQumir::NAst::TExprPtr BuildTopSortMergeProgramAst(
    const std::vector<TSortRadixKeyInput>& keys);

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

struct TJoinHashKernels {
    using TDispatch = std::function<bool(TRowSet* batch, uint64_t* hashes)>;

    TDispatch Left;
    TDispatch Right;
};

// Compiles qumir core-lang kernel sources to LLVM JIT function pointers.
class TKernelCompiler {
public:
    explicit TKernelCompiler(std::ostream* diagnostics = nullptr)
        : TKernelCompiler(TKernelCompilerOptions{.Diagnostics = diagnostics})
    {}

    explicit TKernelCompiler(TKernelCompilerOptions options)
        : Diagnostics_(options.Diagnostics)
        , Stage_(std::move(options.Stage))
        , Operator_(options.Operator)
        , Sink_(options.Sink)
        , BindNow_(options.BindNow)
    {
        Opts_ = KernelRunnerOptions();
        Opts_.NativeCode = true;
        Opts_.EnablePerfJitEventListener = true;
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

    using TSortRadixCompositeDispatch = std::function<void(
        TRowSet* store, int64_t* rowIds, int64_t* work, uint32_t* counts,
        int64_t n, bool* descs)>;

    using TSortRadixCompositeNullableDispatch = std::function<void(
        TRowSet* store, int64_t* rowIds, int64_t* work, uint32_t* counts,
        int64_t n, bool* descs, bool* nullsFirsts)>;

    using TTopSortDispatch = std::function<int64_t(
        TRowSet* state,
        TRowSet* batch,
        int64_t* rowIds,
        int64_t* work,
        uint32_t* counts,
        int64_t n,
        uint8_t* pickSrc,
        uint32_t* pickIdx,
        int64_t limit)>;

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
    TFilterDispatch CompileFilter(const NKernel::TOperatorKernelSpec& spec);

    // Compiles a project kernel for the computed (non-ident) projections.
    // computedExprs and computedTypes are parallel; computedTypes[k] is the
    // inferred output type of computedExprs[k] (see InferProjectExprType). The
    // kernel writes each expression to its output buffer, casting to that type.
    TProjectDispatch CompileProject(const NKernel::TOperatorKernelSpec& spec);

    TSortRadixCompositeDispatch CompileRadixSortComposite(
        const std::vector<TSortRadixKeyInput>& keys);
    TSortRadixCompositeNullableDispatch CompileRadixSortCompositeNullable(
        const std::vector<TSortRadixKeyInput>& keys);
    TTopSortDispatch CompileTopSort(
        const std::vector<TSortRadixKeyInput>& keys);

    // Compiles the per-query generic update and finalize programs for `aggs`
    // grouped by `groupKeys`, over rows of `inputType`.
    //
    // Current aggregate-value constraints (NQumir::TError if violated):
    // - one or more integer/f64/string group keys;
    // - agg.Func in {count, sum, min, max};
    // - every agg with Arg references the same single integer column;
    // - all reducer states are i64. count(*) aggs (Arg == nullptr) pass 0.
    TAggregateKernels CompileAggregate(
        const NKernel::TOperatorKernelSpec& spec);

    // Compiles the symmetric hash join kernels described by `spec`.
    TJoinKernels CompileJoin(
        const NKernel::TOperatorKernelSpec& spec);

    // Compiles rowset-wide hash helpers for the join keys described by `spec`.
    // Each dispatch fills one uint64 hash per physical input row. Selection is
    // not applied by the helper; callers skip unselected rows during scatter.
    TJoinHashKernels CompileJoinHash(
        const NKernel::TOperatorKernelSpec& spec);

private:
    // Implementation for the equi-key pairs `keys` (parallel left/right column
    // names) over rows of leftType/rightType. String keys are not supported yet.
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

    // Wrap a freshly built kernel program: create its slot, append to Sink_,
    // and (if BindNow_) JIT-finalize immediately. Returns the emitted kernel
    // (its Slot backs the caller's dispatch closures).
    TGeneratedKernel EmitKernel(
        std::string name,
        std::vector<std::string> entrypoints,
        NQumir::NAst::TExprPtr ast,
        std::shared_ptr<void> storage = nullptr);

    std::ostream* Diagnostics_ = nullptr;
    std::string Stage_;
    const void* Operator_ = nullptr;
    std::vector<TGeneratedKernel>* Sink_ = nullptr;
    bool BindNow_ = true;
    NQumir::TLLVMRunnerOptions Opts_;
};

} // namespace NQdb
