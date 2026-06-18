# Join architecture

This document describes the current physical join implementation. Development
history lives in `PLAN_JOIN.md`, `PLAN_RESIDUAL_FILTER.md`, and
`PLAN_JOIN_STREAMING.md`; this file is the runtime contract.

## Overview

QDB implements equi-joins with a JIT-compiled symmetric hash join. The runtime
owns row storage, hash table headers, pair buffers, and output materialization.
Reusable Oz code implements the generic hash-table probe/insert logic, while
C++ generates query-specific key extraction, key hash/equality functions, and
optional residual predicates.

The important performance property is adaptive streaming after one input side
reaches EOF:

- while both inputs are still alive, both sides can be buffered and inserted;
- once one side reaches EOF, it becomes the build/materialized side;
- the remaining side switches to probe-only streaming;
- streamed rows are not inserted into their own hash table and are not retained
  in a `TRowStore`;
- output involving a streamed row is materialized immediately, while that
  stream batch is still alive.

This gives the join a CBO-free way to avoid materializing the large side when
the smaller side naturally finishes first.

## Source map

| Area | Source |
|---|---|
| Logical join operator and output schema | `qdb/ops/join.{h,cpp}` |
| Physical planning | `qdb/exec/planner.cpp` |
| Runtime executor and output materialization | `qdb/exec/join_exec.{h,cpp}` |
| Join key descriptor | `qdb/kernel/join_key.{h,cpp}` |
| Generated join process/probe/finalize AST | `qdb/kernel/join_gen.{h,cpp}` |
| Residual predicate AST generation | `qdb/kernel/gen.{h,cpp}` |
| JIT compilation and C++ callable ABI | `qdb/kernel/compiler.{h,cpp}` |
| Generic Oz join table/update code | `qdb/kernel/join/*.oz` |
| Shared hash table ABI | `qdb/modules/qumirdb.cpp` |
| Runtime tests | `test/test_join_exec.cpp` |
| Kernel tests | `test/test_join_kernel.cpp` |
| Planner tests | `test/test_join_planner.cpp` |

## Supported join forms

Current production support:

| Join type | Status | Notes |
|---|---|---|
| `Inner` | Supported | Adaptive symmetric/probe-only hash join |
| `LeftSemi` | Supported | Left-preserving, emits left columns only |
| `LeftAnti` | Supported | Left-preserving, emits left columns only |
| `Left` | Supported | Symmetric path plus unmatched-left final scan |
| `Right` | Supported | Symmetric path plus unmatched-right final scan |
| `Full` | Not implemented in runtime | Logical output typing exists, runtime guard rejects it |
| `RightSemi` / `RightAnti` | Not implemented in runtime | Logical enum exists, runtime guard rejects it |
| Cross join | Supported for `Inner` only | Separate `TRuntimeCrossJoin` path |

Residual predicates are supported for `Inner`, `LeftSemi`, and `LeftAnti`.
They are intentionally rejected for outer joins until null-padding semantics and
unmatched-row tracking are handled inside the residual path.

String join keys are not supported by the current production `CompileJoin`
validation. Fixed-width integer and `f64` keys are supported, including
composite key structs generated from multiple equi-key pairs.

## Output schema

`ComputeJoinOutputType` defines the logical output shape:

- `Inner`: all left columns followed by all right columns;
- `LeftSemi` / `LeftAnti`: left columns only;
- `RightSemi` / `RightAnti`: right columns only, typed but not executed;
- `Left`: left columns followed by nullable right columns;
- `Right`: nullable left columns followed by right columns;
- `Full`: nullable columns from both sides, typed but not executed.

For joins that emit both sides, duplicate column names are an error. The
column-qualification pass normally prevents accidental collisions by rewriting
source fields to qualified names such as `lineitem.l_partkey`.

Column pruning runs before physical planning. Each join child keeps:

- its equi-key columns;
- columns referenced by the residual predicate on that side;
- columns required by the parent and belonging to that side.

The physical planner then computes runtime output types from the pruned child
schemas.

## Runtime data structures

`TRowStore` owns retained input batches for one join side. Stored rows are
addressed by a packed `TRowId`:

```text
row_id = (batch_idx << 32) | row_idx
```

`kNullRowId == -1` marks absent rows for outer-join null padding and for
semi/anti builder pairs where the non-emitted side is ignored.

`TJoinOutputBuilder` stores `(left_row_id, right_row_id)` pairs and later
gathers output columns from `LeftRows_` and `RightRows_`. This is used when
both sides of a pair are retained.

Probe-only streaming uses a different materialization path. The streamed row
does not have a stable `TRowStore` entry, so `TRuntimeJoin` drains the kernel
pair buffer immediately into owned output `TRowSet`s and queues them in
`ReadyOutput_`. The stream batch can then be released safely.

## Hash tables and pair buffers

Join reuses the generic `HashTable` external ABI used by aggregation, but with
join-specific dense buffers:

```text
AggBuffers[0] = bucket row count per dense slot
AggBuffers[1] = bucket row capacity per dense slot
AggBuffers[2] = bucket data pointer, stored as i64
```

Each side has its own `HashTable` buffer:

```text
LeftTable_
RightTable_
```

The kernel emits matches into a `PairBuffer` containing adjacent left/right
row-id pairs:

```text
Data[2*i + 0] = left row id
Data[2*i + 1] = right row id
```

The pair buffer grows in Oz code and is destroyed by the compiled kernel
library. C++ resets only `Count` after draining.

## Kernel split

`TKernelCompiler::CompileJoin` composes one complete Qumir AST per entry point:

1. generated key type declarations;
2. generated `rh_hash(Key)` and `rh_key_equal(Key, Key)`;
3. generic join library from `qdb/kernel/join/*.oz`;
4. generated entry wrappers for the concrete query.

The compiled `TJoinKernels` ABI contains:

| Entry | Purpose |
|---|---|
| `Init(table, capacity)` | Initialize one side's hash table |
| `ProcessLeft(own, opp, batch, batch_idx, pairs, left_store, right_store)` | Probe right, emit matches, insert left rows |
| `ProcessRight(own, opp, batch, batch_idx, pairs, left_store, right_store)` | Probe left, emit matches, insert right rows |
| `ProbeLeftStream(build, batch, batch_idx, pairs, left_store, right_store)` | Probe right/build table with a left stream batch; no insert |
| `ProbeRightStream(build, batch, batch_idx, pairs, left_store, right_store)` | Probe left/build table with a right stream batch; no insert |
| `InsertKeyOnly(own, opp, batch, batch_idx, pairs)` | Build a key-existence table for non-residual semi/anti |
| `FinalizeAntiSemi(own, opp, pairs)` | Scan left table and emit semi/anti left row ids |
| `FinalizeOuter(own, opp, pairs)` | Scan own side and emit rows with no opposite match |
| `DestroyTable(table)` | Free table allocations |
| `DestroyPairs(pairs)` | Free pair-buffer allocation |

The generic Oz helper `jt_probe_and_emit` does the common probe and pair emit.
`jt_emit_and_insert` calls it first, then inserts the current row into the
own-side table. Probe-only wrappers call only `jt_probe_and_emit`.

This split is deliberate: the expensive and type-specialized key extraction is
generated once per side, while the generic table code remains shared.

## Residual predicates

A residual predicate is a non-key predicate over the joined row, for example
`l1.l_suppkey != l2.l_suppkey` in TPC-H Q21.

Residual filtering happens in the join kernel before `pb_push`. The compiler
replaces the default always-true Oz function:

```text
jt_residual_filter(left_store, right_store,
                   stream_left_batch, stream_right_batch,
                   left_row_id, right_row_id) -> bool
```

with a generated predicate function. It decodes row ids, materializes only the
referenced columns, evaluates SQL three-valued truth rules for nullable values,
and returns a boolean.

Stored rows are read from `left_store[batch_idx]` or `right_store[batch_idx]`.
For probe-only streaming, the generated probe wrapper passes `batch_idx == -1`
for the streamed side. The residual function interprets that sentinel as:

```text
left batch  == -1 -> read left columns from stream_left_batch
right batch == -1 -> read right columns from stream_right_batch
```

This keeps residual evaluation inside the kernel without retaining the streamed
side.

## Scheduling

`TRuntimeJoin` has three inner-join modes:

```text
Symmetric
StreamLeftAgainstRight
StreamRightAgainstLeft
```

In `Symmetric` mode, both sides are still live. A pulled batch is retained in
that side's `TRowStore`, processed by `ProcessLeft` or `ProcessRight`, and
inserted into that side's hash table.

The scheduler is row-balanced, not simple batch round-robin. It tracks:

```text
StoredLeftRows_
StoredRightRows_
LastLeftBatchRows_
LastRightBatchRows_
```

and prefers the side with fewer stored rows unless pulling one more batch of
that side would likely catch up to or exceed the other side. This matters for
TPC-H because Parquet batches can be large and join output batches can be
small. A naive batch round-robin can materialize most of a large fact table
before the small side reaches EOF. Row-balanced scheduling lets the small side
finish, then switches the large side to probe-only streaming.

When one side returns EOF:

- `LeftDone_` or `RightDone_` is set;
- if the other side is still live, `StreamMode_` switches to the corresponding
  probe-only mode;
- remaining batches from the live side call `ProbeLeftStream` or
  `ProbeRightStream`;
- the stream batch is materialized into owned output immediately and released.

`BothDone_` becomes true only when the build side has reached EOF and the
stream side has also finished.

Outer joins currently keep the older symmetric scheduling path. They need the
retained opposite table for final unmatched scans, so probe-only streaming is
not enabled there.

## Execution by join type

### Inner

`Inner` uses the adaptive scheduler described above. Stored/stored matches go
through `TJoinOutputBuilder`. Stream/stored matches are materialized
immediately into `ReadyOutput_`.

### LeftSemi / LeftAnti without residual

The non-residual path is blocking but memory-efficient:

1. consume all left batches into `LeftRows_` and `LeftTable_`;
2. consume right batches through `InsertKeyOnly` into `RightTable_`, releasing
   each right batch immediately;
3. call `FinalizeAntiSemi(LeftTable_, RightTable_, pairs)`;
4. emit left rows from the resulting pair buffer.

The right side is never retained because only key existence is needed.

### LeftSemi / LeftAnti with residual

Residual semi/anti cannot use `InsertKeyOnly`, because the residual predicate
may reference right-side non-key columns. The current path:

1. consumes all left batches into `LeftRows_` and `LeftTable_`;
2. streams every right batch through `ProbeRightStream`;
3. the generated residual predicate reads left rows from `LeftRows_` and right
   rows from the transient stream batch;
4. filtered pairs contribute their left row ids to `MatchedLeftIds_`;
5. the executor emits each left row once based on membership.

The right side is not retained and is not inserted into `RightTable_`.

### Left / Right outer

Outer joins use symmetric probe+insert while inputs are live, then call
`FinalizeOuter` on the preserved side:

- `Left`: `FinalizeOuter(LeftTable_, RightTable_, pairs)`;
- `Right`: `FinalizeOuter(RightTable_, LeftTable_, pairs)`, then pair halves
  are swapped before materialization.

The finalize kernel emits rows with no opposite-side match as `(row_id, -1)`.
`TJoinOutputBuilder` turns `kNullRowId` into null output values.

### Cross join

`TRuntimeCrossJoin` is separate from hash join. It buffers the entire right
side, then streams left batches and emits the Cartesian product through
`TJoinOutputBuilder`. Cross join is accepted only for `Inner`.

## Memory and lifetime invariants

The implementation relies on these invariants:

- a `TRowId` with non-negative batch index always refers to a retained
  `TRowStore` batch;
- a `TRowId` with batch index `-1` is valid only during one probe-only kernel
  call and only for residual evaluation/materialization from the stream batch;
- stream-side output is copied into owned output buffers before `Release` of
  the stream batch;
- `TRowStore::Data()` can be invalidated by vector growth, so C++ passes fresh
  store bases on every kernel call;
- `PairBuffer_.Count` is reset after each drain, but allocated capacity is
  reused;
- `ReadyOutput_` owns already-materialized output rowsets and is drained before
  pulling more input;
- the `TRuntimeJoin` destructor releases any queued `ReadyOutput_` batches if
  the consumer stops early.

## Current limitations and likely next work

The largest remaining limitations are:

- no runtime `Full`, `RightSemi`, or `RightAnti`;
- no residual predicates for outer joins;
- no string join keys;
- outer joins do not use adaptive probe-only streaming;
- intermediate join output is still materialized as full rowsets between
  operators rather than fused directly into a following project/aggregate.

For TPC-H Q9, the current row-balanced scheduler is critical: it avoids
materializing almost all of `lineitem` before the small filtered part/partsupp
side reaches EOF. Further performance work is likely to focus on reducing
intermediate materialization after matches rather than on hash insertion.
