# Sort Plan

Goal: add SQL `ORDER BY` support through a logical sort operator, a runtime
pipeline breaker, and Qumir kernels for stable key sorting. The prototype in
`test/test_sort.cpp` is the starting point: stable radix sort over row indices,
applied from the least significant sort key to the most significant sort key.

## Scope

- [x] MVP supports `ORDER BY` on output columns after the final projection.
- [x] MVP supports fixed-width numeric keys: unsigned/signed integers and `f64`.
- [x] MVP preserves row stability for equal keys.
- [x] MVP honors `ASC`/`DESC`.
- [x] MVP honors explicit `NULLS FIRST` / `NULLS LAST` in the runtime comparator.
- [x] MVP supports string keys through C++ stable sorting.
- [ ] Later phase wires `LIMIT` / `OFFSET` above sort, not into the first sort kernel.
- [ ] Top-sort optimization supports bounded `ORDER BY ... LIMIT N` without
  materializing and sorting the full input.

## Current Starting Point

- SQL parser already records `TSqlQuery::OrderBy`, `Limit`, and `Offset`.
- `BuildQuery` currently has a TODO for `ORDER BY / LIMIT / OFFSET`.
- `test/test_sort.cpp` already prototypes:
  - radix key normalization for signed integers;
  - radix key normalization for `double`;
  - stable count-sort pass over `indices`;
  - multi-column ordering by sorting indices from the last key to the first key.
- qdb-local Oz radix library exists in `qdb/kernel/sort/radix.oz` with
  type-overloaded `qumir_radix_key` helpers and generic index sorting.
- qdb-specific wrappers live in `qdb/kernel/sort/radix_wrappers.oz` and call the
  generic radix functions through `use radix`.
- Runtime nodes already use pull-based `IRuntimeNode::Next(TRowSet&)`.
- Aggregation and join already establish the pattern for pipeline breakers.

## Design Decisions

- [x] Represent sort as a schema-preserving logical operator:
  `TSortOperator(input, vector<TSortKey>)`.
- [x] Build sort after the final projection for top-level `ORDER BY`, so aliases
  and projected column names are visible.
- [x] For MVP, require every `ORDER BY` expression to resolve to an output column
  identifier. If the expression is not an identifier, insert a hidden projection
  below sort in a later phase.
- [x] Runtime sort consumes the full input before producing the first output rowset.
- [x] Runtime sort stores retained input batches plus a global permutation vector
  of row ids.
- [x] Output batches are gathered according to the permutation. Start with copied
  output buffers for correctness; add zero-copy slice optimization later only if
  needed.
- [x] Kernel contract sorts only row indices/permutation; it does not reorder
  column buffers in place.
- [x] Keep the public radix implementation generic over value type, selected by
  Qumir overload resolution, with thin qdb-specific wrapper entrypoints.

## Logical Plan

- [x] Add `qdb/plan/ops/sort.h`.
- [x] Add `qdb/plan/ops/sort.cpp`.
- [x] Define:
  - `enum class ESortDirection { Asc, Desc };`
  - `enum class ESortNulls { Default, First, Last };`
  - `struct TSortKey { std::string Column; ESortDirection Direction; ESortNulls Nulls; };`
  - `class TSortOperator : public IOperator`.
- [x] Implement `RelName() == "sort"`.
- [x] Implement `Children()` with the single input.
- [x] Implement `ComputeReferencedColumns()` as all sort key columns.
- [x] Keep default `RequiredColumnsForChild`: needed output columns plus sort keys.
- [x] Implement `ToString()` as `(rel sort <input> (<col> asc nulls-last) ...)`.
- [ ] Add `MakeSort(...)` only if tests need hand-built plans; otherwise keep the
  constructor path first.
- [ ] Update `CloneOperatorExprs` if sort keys later contain expressions. For the
  MVP string-column keys need no clone handling.
- [x] Update `AnnotateTypes`: input schema and output schema are identical.
- [x] Update `ApplyColumnPruning`: no special case needed because default
  `RequiredColumnsForChild` keeps needed output columns plus sort keys.
- [x] Add a regression test to prove sort keys are not pruned away.
- [x] Update S-expression printer support for `(rel sort ...)`.
- [ ] Update S-expression parser support if hand-written regtest plans
  need `(rel sort ...)`.

## SQL Builder

- [x] In `BuildQuery`, first build the `SELECT` body as today.
- [x] If `query.OrderBy` is present, wrap the built plan in `TSortOperator`.
- [x] Resolve each `TSqlOrderItem` against the current output schema.
- [x] For MVP accept only `TIdentExpr` order items.
- [x] Decide default null ordering explicitly:
  - [x] `ASC` defaults to `NULLS LAST`.
  - [x] `DESC` defaults to `NULLS FIRST`.
  - [x] Resolve defaults in runtime sort comparison.
- [x] Add a precise error for unsupported `ORDER BY` expressions.
- [x] Add regtests:
  - [x] `ORDER BY a ASC`;
  - [x] `ORDER BY a DESC`;
  - [x] `ORDER BY alias`;
  - [x] `ORDER BY a NULLS FIRST`;
  - [x] `ORDER BY a NULLS LAST`;
  - [x] multi-key `ORDER BY a, b DESC`.
- [ ] Keep `LIMIT` / `OFFSET` as a follow-up unless the executor already has a
  natural place for a limit node.

## Runtime Node

- [x] Add `qdb/exec/sort_exec.h`.
- [x] Add `qdb/exec/sort_exec.cpp`.
- [x] Add `TRuntimeSort : public IRuntimeNode`.
- [x] Constructor inputs:
  - child runtime node;
  - output type;
  - key descriptors with physical column indices, type info, direction, nulls.
- [x] Add numeric compiled sort kernels as an internal backend of `TRuntimeSort`.
- [x] Internal state:
  - [x] `bool Materialized_`;
  - [x] retained input batches;
  - [ ] prefix row offsets per batch;
  - [x] global `TRowId` permutation;
  - [ ] work permutation buffer;
  - [x] output cursor.
- [x] Use `uint32_t` row ids for MVP with an explicit row-count limit; upgrade to
  `uint64_t` if total input rows can exceed `UINT32_MAX`.
- [x] During materialization:
  - [x] pull all input batches;
  - [x] retain each input batch in `TRowStore`;
  - [x] initialize global row-id permutation from selected input rows;
  - [x] run stable lexicographic index sort over the configured keys.
- [x] During output:
  - [x] gather rows from retained batches into owned output columns;
  - [x] respect a configurable output batch size;
  - [x] release retained input batches when `TRuntimeSort` is destroyed.
- [x] Implement gather helpers for fixed-width columns.
- [x] Implement gather helpers for nullable masks.
- [x] Implement gather helpers for bool bit-packed data.
- [x] Implement variable-length string compare and gather; string keys use C++
  stable index sorting.
- [x] Add `TPhysicalPlanner::PrintRuntimePlan` entry: `sort [stable indices]`.
- [x] Add `TPhysicalPlanner::Build` entry before the final unknown-operator error.
- [x] Update CMake for new exec files.

## Top Sort

- [x] Add a C++ prototype in `test/test_sort.cpp` before changing runtime code:
  - [x] keep `state` as the current sorted top-N rows;
  - [x] radix-sort each incoming batch into a temporary index array;
  - [x] merge sorted `state` and sorted temporary batch into a selector array:
    `Pick { uint8_t src; uint32_t idx; }`, where `src = 0` means old state and
    `src = 1` means temporary batch;
  - [x] gather the next `state` from selector in a separate pass.
  - [x] prototype merge/gather helpers take caller-owned pointer buffers; buffer
    resizing stays outside hot helpers to match the intended runtime shape.
  - [x] group current `state` and reusable scratch buffers into a
    `TTopSortScratch`-style entity.
  - [x] add an N-column prototype where one selector is built from the sort key
    and then applied by one gather pass per output column.
- [ ] Add a logical top-sort representation for `ORDER BY ... LIMIT N`:
  - [x] either introduce `TTopSortOperator`, or annotate `TSortOperator` with a
    bounded limit once `LIMIT/OFFSET` nodes exist;
  - [x] add a logical `TLimitOperator` so top-sort can be introduced by a pass
    over `limit(sort(...), N, 0)`;
  - [x] add `ApplyTopSort` pass that rewrites `limit(sort(input), N, 0)` into
    `top-sort(input, keys, N)`;
  - [ ] keep full `TSortOperator` as the fallback for unbounded sorts and for
    `OFFSET` cases that cannot be bounded cheaply.
- [ ] Add a runtime top-sort node:
  - [x] keep bounded state and reusable temporary buffers in `TTopSortScratch`;
  - [x] maintain owned sorted `state` of at most `N` rows;
  - [x] for every incoming batch, materialize key scratch arrays only for selected
    batch rows;
  - [x] radix-sort the batch indices using the existing fused numeric kernels;
  - [x] build a `Pick` selector by merging `state` and temporary sorted indices;
  - [x] gather a new owned `state` from selector after merge;
  - [x] return `state` in output batches after input is exhausted.
  - [ ] Remove silent runtime fallbacks from top-sort:
    - [ ] keep only the string-key C++ sorting path as an intentional fallback;
    - [ ] numeric/radix-sortable keys must use radix kernels;
    - [ ] unsupported or degenerate cases, including more than `UINT32_MAX`
      batch rows for radix indices, should fail explicitly instead of falling
      back to full C++ sort.
- [ ] Add top-sort kernel/runtime support:
  - [x] add `qdb/kernel/sort/top_sort.oz` with generic selector merge and
    per-column gather helpers;
  - [x] reuse existing radix sort kernels for per-batch sorting;
  - [x] first implement merge selector construction in C++;
  - [ ] consider an Oz merge-selector kernel after ABI and performance are clear.
- [ ] Add tests:
  - [x] prototype test with mixed old-state and temp picks;
  - [x] runtime top-sort with one batch larger than limit;
  - [x] runtime top-sort across multiple batches;
  - [x] stable equal-key behavior across state/temp merge;
  - [x] nullable numeric keys with `NULLS FIRST/LAST`;
  - [x] Oz top-sort merge/gather helper smoke test;
  - [ ] SQL/regtest coverage for `ORDER BY ... LIMIT`.

## Kernel API

- [ ] Add `TSortKernels` to `qdb/kernel/compiler.h`.
- [x] Add `TKernelCompiler::CompileRadixSortIndices(...)`.
- [x] Add `TKernelCompiler::CompileRadixSortComposite(...)` for fused numeric
  composite-key radix kernels.
- [ ] Add a higher-level `TSortKernels`/`CompileSortKey(...)` descriptor once
  nullable policy and rowset-key ABI are finalized.
- [x] Kernel ABI MVP:
  `sort_key_pass(rowsets, batchOffsets, batchCount, indices, work, rowCount, digit)`.
- [x] Decide rowset storage ABI for the first numeric backend:
  - [ ] Option A: pass contiguous `TRowSet*` and `int64_t* batchOffsets`.
  - [x] Option B: pre-materialize key columns into contiguous scratch arrays in C++,
    then kernel only sorts scratch keys.
- [x] Prefer Option A if Qumir code stays readable; choose Option B if Qumir
  indexing into batch arrays becomes too awkward or slow.
- [x] Generate one fused kernel per numeric sort operator, with:
  - [ ] key physical column index;
  - [x] key type;
  - [x] radix byte count;
  - [x] direction;
  - [x] null ordering;
  - [x] row id width: `u32` indices for radix path, `TRowId` for retained rows.
- [x] Add qdb-local numeric radix normalization:
  - [x] unsigned integers: identity;
  - [x] signed integers: flip sign bit;
  - [x] `f64`: bitcast to `u64`, then use the prototype's sign-mask transform;
  - [x] descending: reverse bucket placement while
    preserving stability.
- [x] Add nullable radix handling as a separate Oz kernel:
  - [x] compute null bucket independent of value bits;
  - [x] for non-null rows sort by normalized value;
  - [x] for null rows keep stable relative order;
  - [x] make `NULLS FIRST/LAST` independent of direction after defaults are resolved.
- [x] Keep count array size at 256 buckets per byte pass for non-nullable radix
  and 257 buckets for nullable radix to reserve one null bucket.
- [x] Add a source function for zeroing counts.
- [x] Avoid stack allocations larger than Qumir/LLVM reliably handles; if needed,
  keep `counts[256]` as caller-owned scratch.
- [ ] Add kernel diagnostics labels `sort.<column>`.

## Qumir Source Modules

- [x] First implement qdb-local kernels in `qdb/kernel/sort/*.oz` to avoid coupling
  the first qdb iteration to a Qumir API decision.
- [x] Add `ReadSortKernel(...)` for qdb-local sort libraries.
- [x] Split generic radix code from qdb wrapper entrypoints:
  `radix.oz` has reusable functions without the `qdb_` prefix, while
  `radix_wrappers.oz` exposes qdb test/runtime-facing names.
- [ ] Add `BuildSortKernelLibrary()` once multiple sort libraries need composition.
- [ ] Once the qdb sort kernel stabilizes, evaluate moving generic pieces into a
  Qumir source module.
- [ ] Candidate Qumir module contents:
  - radix key normalization helpers for integer and float bit patterns;
  - count-prefix helpers;
  - stable scatter helpers over index arrays.
- [ ] Candidate module path:
  - `externals/qumir/qumir/modules/radix_sort.oz` if it is general-purpose;
  - keep qdb-specific rowset access in qdb if it depends on `QumirDbModule`.
- [ ] Required Qumir integration check:
  - [ ] source modules can be loaded into a `CompileKernelAst` path, not only parsed
    independently by `TSourceModuleLoader`;
  - [ ] module exports can be merged in dependency-first order without name conflicts;
  - [ ] `use radix_sort` works in core `.oz` libraries compiled by qdb.
- [ ] If Qumir does not yet expose a convenient "load source module and merge AST"
  API for JIT clients, add one in Qumir rather than reimplementing the loader in qdb.
- [ ] If radix helpers need generic array/pointer code that Qumir cannot express
  cleanly, add the missing Qumir language/runtime support before hardcoding more
  AST in qdb.

## Possible Qumir Changes

- [ ] Expose source module loading/merging to `TLLVMRunner` or a small frontend API
  usable by qdb kernel compilation.
- [x] Ensure external-module generic overload instantiation distinguishes concrete
  integer kinds in `TypeKey` (`i8/u8/i16/u16/i32/u32/i64/u64`).
- [ ] Ensure imported source modules participate in name resolution before generic
  specialization.
- [x] Confirm `bitcast` supports every needed scalar path used by radix tests:
  - [x] `f64 -> u64`;
  - [x] signed/unsigned same-width integer reinterpret for integer keys.
- [x] Confirm array/local fixed-size allocation supports `u32 counts[256]` or add
  a caller-owned scratch-buffer pattern.
- [x] Confirm pointer indexing on non-identifier expressions is either fixed or
  avoided with explicit temporaries.
- [ ] Consider adding unsigned right shift / bit operations if radix code becomes
  noisy or impossible in Oz.

## Tests

- [x] Keep `test/test_sort.cpp` as algorithm/unit prototype but replace prints with
  assertions.
- [x] Add kernel tests:
  - [x] `uint8_t` ascending;
  - [x] `int8_t` ascending;
  - [x] `uint16_t` ascending;
  - [x] `int16_t` ascending;
  - [x] `uint32_t` ascending;
  - [x] `int32_t` ascending;
  - [x] `double` radix key normalization, including negative values;
  - [x] multi-key stable ordering in the C++ prototype;
  - [x] descending for qdb-local radix wrappers;
  - [x] nullable radix key with `NULLS FIRST/LAST`;
  - [x] empty input;
  - [x] all equal keys.
- [x] Add runtime tests:
  - [x] one input batch;
  - [x] multiple input batches;
  - [x] selected rows from upstream filter;
  - [x] composite numeric keys through fused radix kernel;
  - [x] projection after sort still sees sorted rows;
  - [x] sort after aggregate.
- [x] Add planner tests:
  - [x] `BuildPlan` wraps top-level `ORDER BY` in `TSortOperator`;
  - [x] column pruning keeps sort key columns;
  - [x] physical planner builds `TRuntimeSort`.
- [x] Add SQL/regtest goldens for `ORDER BY`.
- [x] Update TPCH plan goldens after enabling `ORDER BY` sort nodes.
- [ ] Run:
  - [ ] `cmake --build .`;
  - [x] `ctest --output-on-failure -R 'test_sort_plan|test_sort$|test_sexp'`;
  - [x] `ctest --output-on-failure -R 'test_sort_plan|test_sort$|test_sexp|test_project_e2e|test_filter|test_aggregate$'`;
  - [x] `ctest --output-on-failure -R 'test_sort|test_reg|test_project|test_filter|test_aggregate'`;
  - [ ] full `ctest --output-on-failure`.

## Implementation Order

- [x] Phase 1: clean up `test/test_sort.cpp` prototype assertions and document exact
  radix semantics for signed/f64/descending/nulls.
- [x] Phase 1.5: add qdb-local generic Oz radix library, qdb wrappers, and Qumir
  overload regression coverage for concrete integer kinds.
- [x] Phase 2: add logical `TSortOperator`, typing, pruning, SQL builder wrapping,
  and plan/regtest coverage.
  - [x] Logical node, typing, printer, SQL builder wrapping, and unit planner
    coverage are implemented.
  - [x] Regtest goldens need canonization for queries that already contain
    `ORDER BY`.
  - [x] Runtime/physical planner support is implemented.
- [x] Phase 3: add the runtime sort node with the same permutation contract.
  Strings use C++ stable index sorting; numeric radix kernels should plug into
  this node as a backend rather than as a separate fallback path.
- [x] Phase 4: add Qumir numeric sort-key kernels and switch runtime to them.
  - [x] Non-null numeric keys use qdb-local Qumir radix wrappers through
    `TKernelCompiler::CompileRadixSortComposite`.
  - [x] Composite numeric keys compile as one fused Qumir radix entrypoint for
    the whole key list.
  - [x] String keys use C++ stable index sorting in the same runtime node.
  - [x] Nullable numeric keys use a separate 257-bucket Oz radix kernel for null
    ordering.
- [x] Phase 5: add nullable handling and DESC/null policy tests.
- [ ] Phase 6: decide whether generic radix helpers move into Qumir source modules.
- [ ] Phase 7: add `LIMIT` / `OFFSET` runtime node and top-sort optimization.
  - [x] C++ prototype for radix-sorted batch plus selector merge is in
    `test/test_sort.cpp`.
  - [x] Logical `limit` node and `ApplyTopSort` rewrite pass are implemented.
  - [x] Runtime top-sort exists with owned bounded state and C++ per-batch sort.
  - [x] Replace C++ per-batch sort with radix kernels for numeric keys.
  - [ ] Remove remaining non-string top-sort fallbacks and convert degenerate
    radix cases into explicit errors.
- [ ] Phase 8: add string sort support.

## Open Questions

- [ ] What dialect should define default null ordering?
- [ ] Should `ORDER BY` expressions not present in the select list be supported in
  the first version?
- [ ] Do we want stable SQL ordering as a documented guarantee for equal sort keys?
- [ ] Is `uint32_t` enough for permutation row ids in qdb's expected datasets?
- [ ] Should sorting gather full rows immediately, or should the executor learn a
  selection/permutation rowset representation?
- [ ] Should Qumir's source module system become the default way qdb composes Oz
  kernel libraries, replacing manual `ParseFunctionLibrary` merging over time?
