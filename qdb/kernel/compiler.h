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

// Finalizes ht into a complete TRowSet and destroys ht.
using TAggregateFinishRowSet = std::function<int64_t(void* ht, TRowSet* output)>;

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
    TAggregateFinishRowSet FinishRowSet;
    size_t NumAggs = 0;
    std::vector<TAggregateOutputKey> OutputKeys;
    std::vector<TAggregateOutputAgg> OutputAggs;
};

enum class EJoinKernelOp : int64_t {
    Init = 0,
    UpdateLeft = 1,
    UpdateRight = 2,
    StreamLeft = 3,
    StreamRight = 4,
    Finalize = 5,
    Destroy = 6,
};

using TJoinDispatch = std::function<bool(
    void* left,
    void* right,
    TRowSet* batch,
    int64_t batchIdx,
    void* pairs,
    TRowSet* leftStore,
    TRowSet* rightStore,
    int64_t arg,
    int64_t op)>;

// jt_materialize(pairs, leftStore, rightStore, streamLeft, streamRight,
//                start, limit, out) -> i64.
// Materializes up to `limit` pairs starting at `start` into a complete output
// TRowSet allocated inside the kernel (out->Private holds the owners list; the
// caller assigns out->Destroy). streamLeft/streamRight back pairs whose packed
// batch index is -1 (rows probed from a live stream batch); pass any valid
// pointer when no such pairs exist. Returns the number of rows written, 0 when
// the buffer is exhausted (out is left untouched).
using TJoinMaterialize = std::function<int64_t(
    void* pairs,
    TRowSet* leftStore,
    TRowSet* rightStore,
    TRowSet* streamLeft,
    TRowSet* streamRight,
    int64_t start,
    int64_t limit,
    TRowSet* out)>;

// The compiled kernels for one symmetric hash join. Each side's hash map is a
// caller-owned, zero-initialized HashTable buffer (kHashTableSize); the output
// pair buffer is a zero-initialized buffer of kPairBufferSize. The key columns
// and join-kind dispatch are baked into the generated jt_dispatch function.
struct TJoinKernels {
    // jt_dispatch(left, right, batch, batchIdx, pairs, leftStore, rightStore,
    //             arg, op) -> bool.
    // Init uses arg as initial capacity and initializes both hash tables with
    // the generated key size. UpdateRight/Finalize are specialized by the
    // compiled join type, so host code does not select jt_insert_key_only vs
    // jt_process_right or left/right outer finalization itself.
    TJoinDispatch Dispatch;

    // Output materialization: the per-column gather (types, null masks, string
    // payloads, outer-join null padding) is baked into the generated
    // jt_materialize, so host code only cursors over the pair buffer.
    TJoinMaterialize Materialize;
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
