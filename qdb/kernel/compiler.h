#pragma once

#include <qdb/io/io.h>
#include <qdb/ops/aggregate.h>

#include <qumir/parser/type.h>
#include <qumir/runner/runner_llvm.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

// agg_finalize(ref HashTable ht, <ptr i64> outputKeys, <ptr <ptr i64>>
// outputBuffers, i64 outputCapacity) -> i64. outputBuffers must have NumAggs
// entries, each pointing to an int64_t[outputCapacity] buffer, in the same
// order as the `aggs` passed to CompileAggregate. Returns ht.Size (number of
// groups), or -1 if outputCapacity < ht.Size.
using TAggregateFinalize = std::function<int64_t(void* ht, int64_t* outputKeys, int64_t** outputBuffers, int64_t outputCapacity)>;

// The compiled kernels for one Aggregation query: Dispatch handles
// init/update/destroy of the HashTable (via agg_dispatch's op-codes),
// Finalize copies the dense group keys/aggregate buffers to output arrays.
struct TAggregateKernels {
    TAggregateDispatch Dispatch;
    TAggregateFinalize Finalize;
    size_t NumAggs = 0;
};

// Compiles qumir core-lang kernel sources to LLVM JIT function pointers.
class TKernelCompiler {
public:
    // Filter dispatch: called per batch to fill the selection buffer.
    using TFilterDispatch = std::function<void(TRowSet& rowSet)>;

    // sizeof(HashTable) per modules/qumirdb.cpp's layout — callers of
    // CompileAggregate must allocate a zero-initialized buffer this large
    // for `ht`.
    static constexpr size_t kHashTableSize = 104;

    // Compiles a filter kernel for the given input struct type and predicate.
    // Returns a dispatch lambda that calls the compiled kernel.
    TFilterDispatch CompileFilter(
        const NQumir::NAst::TStructType& inputType,
        const NQumir::NAst::TExprPtr& predicate);

    // Compiles the per-query generic update and finalize programs for `aggs`
    // grouped by `groupKeys`, over rows of `inputType`.
    //
    // Stage 1 constraints (NQumir::TError thrown if violated):
    // - exactly one group key, of type i64 (until generated key operations and
    //   output projection are implemented for the remaining descriptor types);
    // - agg.Func in {count, sum, min, max};
    // - every agg with Arg references the same single i64 column (Stage 1:
    //   one shared value column for all aggregates); count(*) aggs
    //   (Arg == nullptr) pass a constant 0 instead.
    TAggregateKernels CompileAggregate(
        const NQumir::NAst::TStructType& inputType,
        const std::vector<std::string>& groupKeys,
        const std::vector<TAggregateSpec>& aggs);
};

} // namespace NQqb
