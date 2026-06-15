#pragma once

#include <qdb/io/io.h>
#include <qdb/ops/aggregate.h>
#include <qdb/ops/join.h>

#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace NQqb {

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
// pair buffer is a zero-initialized buffer of kPairBufferSize. LeftKeyColIdx /
// RightKeyColIdx are the equi-key column positions in each side's schema.
struct TJoinKernels {
    // jt_init(table, capacity) — initializes one side's HashTable. Returns
    // false on allocation failure.
    std::function<bool(void* table, int64_t capacity)> Init;
    // jt_process_batch(own, opp, batch, keyColIdx, batchIdx, isLeft, pairs):
    // probes opp and appends matching pairs, then inserts the batch's rows into
    // own. isLeft (0/1) orders emitted pairs as (left, right).
    std::function<bool(void* own, void* opp, TRowSet* batch, int64_t keyColIdx,
        int64_t batchIdx, int64_t isLeft, void* pairs)> Process;
    std::function<void(void* table)> DestroyTable;
    std::function<void(void* pairs)> DestroyPairs;
    int32_t LeftKeyColIdx = 0;
    int32_t RightKeyColIdx = 0;
};

// Compiles qumir core-lang kernel sources to LLVM JIT function pointers.
class TKernelCompiler {
public:
    explicit TKernelCompiler(std::ostream* diagnostics = nullptr)
        : Diagnostics_(diagnostics)
    {}

    // Filter dispatch: called per batch to fill the selection buffer.
    using TFilterDispatch = std::function<void(TRowSet& rowSet)>;

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

    // Compiles the symmetric hash join kernels for `leftKey == rightKey` over
    // rows of leftType/rightType. Stage 1 constraints (NQumir::TError if
    // violated): type == Inner; leftKey/rightKey name i64 columns.
    TJoinKernels CompileJoin(
        const NQumir::NAst::TStructType& leftType,
        const NQumir::NAst::TStructType& rightType,
        const std::string& leftKey,
        const std::string& rightKey,
        EJoinType type);

private:
    std::ostream* Diagnostics_ = nullptr;
};

} // namespace NQqb
