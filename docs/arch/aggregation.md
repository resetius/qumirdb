# Aggregation architecture

This document describes the current aggregation implementation. The source of
truth is the code referenced below; `PLAN_AGGREGATION.md` describes development
history and future work rather than the runtime contract.

## Overview

Aggregation is a pipeline breaker. It consumes every input `TRowSet`, updates a
query-local Robin Hood hash table, then emits one dense output `TRowSet`.

The implementation is deliberately split into three layers:

1. C++ derives concrete query types and generates query-specific Qumir AST.
2. Reusable Oz code implements the generic hash table and lifecycle over
   `<named Key (template readable mutable)>`.
3. C++ owns execution, the small `HashTable` header, and the result buffers;
   Oz code allocates and frees the table's internal arrays.

There is no C++ implementation of the aggregation algorithm. C++ composes and
JIT-compiles Oz plus generated AST into two named functions:

```text
agg_dispatch(ht, batch, arg, op) -> i64
agg_finalize(ht, key_outputs, aggregate_outputs, capacity) -> i64
```

## Source map

| Area | Source |
|---|---|
| Logical aggregate operator and output schema | `qdb/ops/aggregate.{h,cpp}` |
| Physical planning | `qdb/exec/planner.cpp` |
| Key type and ABI layout construction | `qdb/kernel/aggregate_key.{h,cpp}` |
| Generated hash, equality, reducers and entry wrappers | `qdb/kernel/gen.{h,cpp}` |
| Oz loading and final AST composition | `qdb/kernel/lib.{h,cpp}` |
| LLVM JIT setup and callable kernel wrappers | `qdb/kernel/compiler.{h,cpp}` |
| Generic production Oz kernels | `qdb/kernel/aggregation/*.oz` |
| External Qumir types and allocator functions | `qdb/modules/qumirdb.cpp` |
| Allocator and `f64` bit helper implementations | `qdb/modules/qumirdb_runtime.{h,cpp}` |
| Runtime pipeline breaker | `qdb/exec/aggregate_exec.{h,cpp}` |
| Kernel-level tests | `test/test_aggregation.cpp` |
| End-to-end execution tests | `test/test_aggregate.cpp` |

## Planning and compilation

`TAggregateOperator` stores group column names and `TAggregateSpec` entries.
The physical planner builds its input node, computes the post-pruning input
schema, and calls:

```cpp
TKernelCompiler::CompileAggregate(inputType, groupKeys, aggs)
```

Compilation performs these steps:

1. Validate group columns and aggregate specifications.
2. Build a `TAggregateKeyDescriptor` for the concrete group key.
3. Obtain the external `TColumn`, `TRowSet`, and `HashTable` types from
   `QumirDbModule`.
4. Build an unresolved update program with
   `BuildGenericAggregateProgramAst`.
5. Compile it with overloads enabled and explicit entry `agg_dispatch`.
6. Build a separate finalize program with
   `BuildGenericAggregateFinalizeProgramAst`.
7. Compile it with explicit entry `agg_finalize`.

The two programs use separate `TLLVMRunner` instances. Each returned C++
closure retains its runner, so the JIT code remains alive as long as the
compiled aggregation kernels.

`CompileKernelAst` always receives an explicit entry name. The aggregation
code does not depend on source order or on the runner choosing the first
function.

## Concrete key representation

`BuildAggregateKeyDescriptor` maps logical group columns to a fixed-width key:

- one group column: `Key` is that column's type directly;
- multiple group columns: `Key` is a generated named struct such as
  `AggKey_k1_i64_k2_f64`;
- generated struct fields are named `key_0`, `key_1`, and so on;
- explicit `u8` padding fields preserve the C/LLVM layout;
- each descriptor records source column index, offset, size, and alignment.

Supported layout leaves are integers, `f64`, `bool`, and recursively nested
structs. The current `CompileAggregate` validation is narrower: each selected
group column must itself be an integer or `f64`. Recursive struct hashing is
already implemented and tested, but is not yet exposed by that production
validation path.

Owned key arrays are byte-addressed in the external ABI. `HashTable.Keys` and
`HashTable.GroupKeys` are `<ptr u8>` plus `HashTable.KeySize`. A generic Oz
function casts them locally to `<ptr Key>` only after `Key` is specialized.
This keeps the C++ ABI independent of the query's key type.

## Building the update AST

`BuildGenericAggregateProgramAst` constructs one top-level Qumir block in
dependency order:

```text
optional generated Key type declaration
generated rh_hash(Key)
generated rh_key_equal(Key, Key)
generated reduce_0 ... reduce_N
generated agg_apply_reducers(...)
functions parsed from robin_hood_rehash_generic.oz
functions parsed from aggregation_hashtable_generic.oz
generated agg_dispatch(...)
```

The complete block is unresolved when returned. `TLLVMRunner` resolves it with
`AllowOverloads=true`, instantiates generic named types, lowers it, and compiles
the selected `agg_dispatch` entry.

The generated dispatch wrapper adapts `TRowSet` to the generic table API:

- reads each key column through its concrete pointer type;
- materializes a generated struct value for a composite key;
- reads the one aggregate value column and widens it to `i64`;
- respects `TRowSet.Selection` when it is non-null;
- calls `aht_update(ht, key, value)` once per selected row.

Dispatch operation codes are:

| `op` | Operation | Meaning of `arg` |
|---:|---|---|
| 0 | `aht_init` | initial capacity |
| 1 | batch update | ignored |
| other | `aht_destroy` | ignored |

## Generic hash injection

The Oz table never knows how a key is hashed or compared. It only contains
calls with these names:

```text
rh_hash(Key) -> i64
rh_key_equal(Key, Key) -> bool
```

The concrete overloads are generated by `GenKeyOperationFunDecls` from the
`TAggregateKeyDescriptor` and inserted before the generic Oz library in the
same AST. They do not come from a fixed `key_ops_i64.oz` production dependency.

Specialization works as follows:

1. Generic Oz functions declare parameters as
   `<named Key (template readable mutable)>`.
2. Generated `agg_dispatch` calls `aht_update` with the concrete key value.
3. Qumir infers that template `Key` from the call and instantiates the generic
   function chain.
4. Calls to `rh_hash` and `rh_key_equal` resolve to the generated concrete
   overloads in the merged AST.

This is why `AllowOverloads=true` and compilation of the complete merged AST
are required. The generic libraries and concrete type helpers are not compiled
independently and linked afterward.

### Generated hash rules

The generator recursively walks the AST type rather than the raw memory:

- integers are converted through the corresponding unsigned width, widened to
  `u64`, and mixed with the xorshift/multiply sequence used by the project;
- `f64` uses `qdb_f64_bits`, canonicalizes `+0.0` and `-0.0` to the same bits,
  canonicalizes every NaN payload to one quiet NaN, then uses the same mixer;
- structs hash every non-padding field recursively and combine field hashes in
  declaration order with a boost-style ordered combiner;
- generated `__qdb_padding_*` fields are ignored.

Equality follows the same recursive type walk. Integer leaves use ordinary
equality. `f64` compares canonicalized bits, so all NaNs are equal for hash-key
purposes and signed zeroes are equal. Struct equality is the conjunction of
all non-padding field equalities.

The invariant is strict: if `rh_key_equal(a, b)` is true, both values must have
the same `rh_hash` result.

## Reducer injection

Reducers are also query-specific generated functions. For aggregate number
`i`, `GenReducerFunDecls` creates:

```text
reduce_i(i64 prev, i64 value, bool is_new) -> i64
```

Current bodies implement `count`, `sum`, `min`, and `max`. A generated
`agg_apply_reducers` statically calls every `reduce_i` and stores the result in
`AggBuffers[i][dense_slot]`. The generic table therefore knows only that it
must call `agg_apply_reducers`; it does not contain runtime dispatch on an
aggregate function name.

This generated-function boundary is the intended insertion point for future
custom Oz reducer functions carried by an aggregation AST node.

## Robin Hood table

The production update program loads two Oz files:

- `robin_hood_rehash_generic.oz` implements rebuilding the probe arrays;
- `aggregation_hashtable_generic.oz` implements lookup, insertion, allocation,
  grow, update, and destroy over `<ref HashTable>`.

The table separates probe storage from dense output storage:

```text
probe slot: Keys[slot], Dist[slot], SlotId[slot]
dense slot: GroupKeys[slot_id], AggBuffers[*][slot_id]
```

Robin Hood displacement moves probe entries, including their stable
`SlotId`. Dense keys and aggregate states remain indexed by `SlotId`, so probe
movement does not reorder aggregate state.

Lookup stops on an empty slot or when the resident probe distance is smaller
than the query distance. Insertion swaps with a resident whose distance is
smaller than the carried entry's distance. Capacity is a power of two and the
home slot is `hash & (capacity - 1)`.

Before inserting a new group, `aht_update` grows when the next size would
exceed 75% of capacity. Grow allocates doubled probe and dense arrays, rebuilds
the probe table using the injected hash/equality contract, copies dense keys
and aggregate states, then frees the old arrays.

## HashTable ABI and memory ownership

`QumirDbModule` exposes a 104-byte `HashTable` struct. Its important fields are:

| Field | Type | Purpose |
|---|---|---|
| `Keys` | `u8*` | probe keys, `Capacity * KeySize` bytes |
| `Dist` | `i64*` | probe distance, `-1` means empty |
| `SlotId` | `i64*` | stable dense slot for each probe entry |
| `GroupKeys` | `u8*` | dense keys |
| `AggBuffers` | `i64**` | one dense state array per reducer |
| `Capacity` | `i64` | power-of-two table capacity |
| `Size` | `i64` | number of groups |
| `NumAggs` | `i64` | reducer count |
| `KeySize` | `i64` | concrete key size in bytes |

`Scratch`, `Scratch2`, `QueryKey`, and `NumKeys` are retained in the current
ABI but are not used by the production table path.

Ownership is split intentionally:

- `TRuntimeAggregate` owns a zero-initialized 104-byte header for one query
  execution;
- `aht_init` and `aht_rehash` allocate internal arrays through the registered
  `qdb_alloc` external function;
- `aht_destroy` releases those arrays through `qdb_free`;
- C++ owns the finalized output arrays and their `TRowSet` lifetime.

The allocator functions currently wrap `malloc`/`free`. A future query-owned
allocator can replace this implementation without changing the typed generic
kernel boundary; allocator context propagation is not designed yet.

## Finalization

The finalize program is smaller and separately compiled:

```text
optional generated Key type declaration
functions parsed from aggregation_finalize_states.oz
generated agg_finalize(...)
```

`aht_finalize_states` copies each dense `i64` aggregate buffer. The generated
`agg_finalize` handles concrete key projection:

- C++ passes one opaque `u8*` output buffer per logical key column;
- generated code casts each buffer to the concrete field pointer type;
- scalar dense keys are copied directly;
- composite dense `Key` values are projected from `key_0`, `key_1`, etc. into
  separate columnar output buffers.

This preserves a type-independent C++ ABI while producing a normal SoA
`TRowSet`. Finalize returns the group count, or `-1` when output capacity is
smaller than `HashTable.Size`.

## Runtime execution

`TRuntimeAggregate::Next` runs once:

1. Zero-initialize the `HashTable` header and initialize capacity to 4.
2. Pull every input batch and invoke update dispatch.
3. Read the final group count from the header.
4. Allocate aligned byte buffers for each key output and `i64` buffers for each
   reducer output.
5. Invoke `agg_finalize`.
6. Destroy the hash table through dispatch.
7. Return one `TRowSet`; its private owner releases all output buffers.

The node then reports end-of-stream on subsequent `Next` calls.

## Production and staged Oz files

Only these files are currently composed by `CompileAggregate`:

- `robin_hood_rehash_generic.oz`
- `aggregation_hashtable_generic.oz`
- `aggregation_finalize_states.oz`

Other files under `qdb/kernel/aggregation` are staged components, focused
examples, or earlier integration layers used by `test/test_aggregation.cpp`.
In particular, `key_ops_i64.oz` is a test/reference implementation; production
hash and equality helpers are generated from the query type AST.

## Current constraints

- At least one group key is required.
- Production group columns are fixed-width integers or `f64`.
- Reducer state and output are always `i64`.
- Aggregate arguments must be column references.
- All reducers with an argument must currently use the same integer column.
- `count(*)` supplies zero as its unused value argument.
- Null masks are not part of aggregation semantics yet; only row selection is
  honored.
- Update allocation/insertion failures are represented inside Oz by negative
  slots, but the generated batch wrapper currently returns zero after the
  loop, so runtime error propagation remains incomplete.

## Extension points

The main type-generic extension points are already isolated:

- add a supported leaf type in key layout, hash generation, and equality
  generation together;
- relax `CompileAggregate` validation when recursively structured group
  columns are ready for the input/output ABI;
- replace generated built-in reducer bodies with reducer functions carried by
  the logical aggregation node;
- generalize aggregate state buffers beyond `i64`;
- introduce a query context and query-owned allocator without changing the
  opaque byte storage used for concrete keys.

Any new key type must preserve all three contracts: fixed ABI layout, matching
hash/equality semantics, and typed projection during finalize.
