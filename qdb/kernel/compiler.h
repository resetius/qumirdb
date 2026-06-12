#pragma once

#include <qdb/io/io.h>
#include <qdb/ops/aggregate.h>

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

// agg_finalize(ref HashTable ht, <ptr <ptr u8>> outputKeyBuffers, <ptr <ptr i64>>
// outputBuffers, i64 outputCapacity) -> i64. outputBuffers must have NumAggs
// entries, each pointing to an int64_t[outputCapacity] buffer, in the same
// order as the `aggs` passed to CompileAggregate. Returns ht.Size (number of
// groups), or -1 if outputCapacity < ht.Size. outputKeyBuffers has one opaque
// byte destination per group key; generated code casts each to <ptr Ti>.
using TAggregateFinalize = std::function<int64_t(void* ht, void** outputKeyBuffers, int64_t** outputBuffers, int64_t outputCapacity)>;

struct TAggregateOutputKey {
    size_t Size = 0;
    size_t Alignment = 0;
};

// The compiled kernels for one Aggregation query: Dispatch handles
// init/update/destroy of the HashTable (via agg_dispatch's op-codes),
// Finalize copies the dense group keys/aggregate buffers to output arrays.
struct TAggregateKernels {
    TAggregateDispatch Dispatch;
    TAggregateFinalize Finalize;
    size_t NumAggs = 0;
    std::vector<TAggregateOutputKey> OutputKeys;
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

    // Compiles a filter kernel for the given input struct type and predicate.
    // Returns a dispatch lambda that calls the compiled kernel.
    TFilterDispatch CompileFilter(
        const NQumir::NAst::TStructType& inputType,
        const NQumir::NAst::TExprPtr& predicate);

    // Compiles the per-query generic update and finalize programs for `aggs`
    // grouped by `groupKeys`, over rows of `inputType`.
    //
    // Current aggregate-value constraints (NQumir::TError if violated):
    // - one or more fixed-width integer/f64 group keys;
    // - agg.Func in {count, sum, min, max};
    // - every agg with Arg references the same single integer column;
    // - all reducer states are i64. count(*) aggs (Arg == nullptr) pass 0.
    TAggregateKernels CompileAggregate(
        const NQumir::NAst::TStructType& inputType,
        const std::vector<std::string>& groupKeys,
        const std::vector<TAggregateSpec>& aggs);

private:
    std::ostream* Diagnostics_ = nullptr;
};

} // namespace NQqb
