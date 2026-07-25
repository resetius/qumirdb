# Typed JIT join output gather

## Motivation
Profiling the parallel hash-shuffle join (test/test_shuffle.cpp) showed the JIT
join kernel is ~1.8× slower than a hand-tuned C++ Robin-Hood join, and the LLVM
IR of the probe loop is already optimal. The gap is the **output materialization**:
the kernel emits row-id pairs, then C++ `TakeColumn` gathers each output column
per-row (`store.Column(id)` batch indirection + `memcpy` + null check). The C++
Robin-Hood join instead writes the value directly during probe (one pass).

The fix: generate the gather **inside the JIT**, typed per output column — no
`memcpy`, no runtime width, no separate C++ pass. Reuses existing codegen
primitives: typed column read by decoded row-id (`GenJoinResidualFilterAst` /
`BuildColumnValueAst`) + typed output write (`GenProjectKernelAst`).

## Done — fixed-width (incl. nullable), Inner
- `GenJoinGatherAst` (qdb/kernel/gen.cpp): `jt_gather(left_ids, right_ids, count,
  out, masks, left_store, right_store, stream_left_batch, stream_right_batch)`.
  For pair `i`: decode both row-ids (batch -1 == stream batch), write `out[k][i]`
  typed for each fixed-width output column; for nullable columns also write
  `masks[k]` bit `i` via `qdb_bitmap_set_valid`. Specialized per join at codegen
  (a column emits the mask write only if its type is nullable).
- Compiled in `CompileJoin` when `gatherEligible` (Inner; every output field
  fixed-width — strings still excluded); exposed as `TJoinKernels::Gather`.
- `TJoinOutputBuilder::NextBatch` uses `Gather_` when set, else the C++
  `TakeColumn` path. Allocates a null mask per nullable column.
- `kNullRowId` renamed to `NullRowId`.
- Result: **+22% on the pure-int64 microbench** (real kernel p=8 242→187ms; gap
  to C++ RH 1.8×→1.4×). Correct (35/35 tests, SF10 0 failed).

## The fallback / TPC-H gap
Until strings, a join whose output has **any** string column is fully ineligible
(→ C++ `TakeColumn`). TPC-H join outputs almost always carry a string (names,
flags), so the gather rarely activates there — TPC-H is **neutral** so far.

## Roadmap
1. ✅ **Nullable fixed-width** — done (null mask via `qdb_bitmap_set_valid`).
2. **Strings — per-column hybrid** (in progress): string copy is `memcpy` either
   way, so typed codegen buys nothing for the payload. Instead make eligibility
   **per column, not per join**: the kernel gathers the fixed-width columns, the
   C++ `TakeColumn` fills the string columns. A mixed int+string join (the common
   TPC-H shape) then gets the fast path for its fixed columns instead of falling
   back entirely. Kernel skips string columns in codegen; `NextBatch` fills them
   with `TakeColumn` after the kernel call.
3. **Left/Right outer**: unlocked by the null-mask path — decode `NullRowId` to a
   cleared validity bit; just needs eligibility widened past Inner.
4. **Semi/Anti**: left-only output variant (emit only left columns).

## Root cause of the Q21 crash — FIXED (no crutch)
The crash was **not** streaming pairs: it was **semi/anti joins with a residual
filter**. Those are compiled with an Inner `type` (so the residual machinery is
available), but they emit **left-only** rows with `NullRowId` on the right. The
gather eligibility keyed off that Inner `type`, so it wrongly enabled the
inner (left ++ right) gather, which then dereferenced the absent right side →
SIGSEGV.

Fix: `CompileJoin` now gates the gather on the **original** `spec.JoinType ==
Inner` (new `allowGather` argument), independent of the residual-driven
`kernelType`. The temporary `NextBatch` id-range guard was removed. Q21 (SF10,
w18, sexpr) passes; 35/35 unit tests; 0 TPC-H failures.

## Honest performance result
On TPC-H the gather is **neutral**: SF10 27.57s with vs 27.77s with
`QDB_NO_GATHER` (noise). The earlier apparent −13% was an artifact — a crashing
Q21 contributes 0s to the harness total. The gather's +22% shows only on the
pure-fixed-width microbench; TPC-H join outputs carry strings (C++ path) and the
join output materialization is not the TPC-H bottleneck. Next perf work should
profile SF10 afresh to find the real hotspot (likely scan / lowering / aggregate,
not the join).

## (obsolete) TEMPORARY crutch to remove — stream-side pairs in the Builder gather
Wiring the gather revealed a **crash on Q21** (SF10, sexpr): the JIT gather
resolves row IDs against the two row *stores* only. But a build-one-side /
stream-other-side inner join emits matched pairs `(leftStored, rightStream)`
where the stream side is packed with **batch index −1** and its rows are **never
stored** (observed: `rb=−1, rightBatches=0`, left fully stored). Those pairs still
reach `TJoinOutputBuilder`, so the gather dereferences the empty right store →
null column `Data` → SIGSEGV.

Current stopgap (`join_exec.cpp`, `NextBatch`): scan the batch's IDs and fall back
to C++ `TakeColumn` if any references a batch outside `[0, BatchCount)`. **This is
a crutch and must go.**

### Proper solution (planned)
1. **Pin the path**: identify exactly which `TInnerJoinProcessor` mode routes
   stream-side pairs into the `Builder` instead of `DrainStreamingPairs` (which
   already gathers the stream side from the live batch via `TakeColumnFromBatch`).
   Determine whether that is a routing bug or intended.
2. If **routing bug** → send those pairs to `DrainStreamingPairs`; the Builder
   then only ever holds fully-stored pairs and the gather is unconditionally
   valid (guard deleted).
3. If **by design** → add a typed **streaming gather** kernel `jt_gather_stream`
   (mirrors `jt_gather` but reads the stream side from a passed current
   `TRowSet&` by `RowIndex`, the stored side from the store) and use it in
   `DrainStreamingPairs`. Then both output paths are typed and the guard is
   deleted. This also closes the last C++ materialization path.
4. Add a regression test reproducing the Q21 shape (build-left / stream-right
   inner join whose output is gathered) so the crash cannot return.

## Related: shuffle materialization codegen (crutch in place)
`THashShuffleTask::FlushBuffer` (multi-batch path) materializes each output column
with a per-row copy. A **crutch** now type-specializes the common fixed widths
(8/4/2/1) with a load/store instead of a `memcpy` call. The proper fix is the same
as the join gather: a **codegen'd, per-schema shuffle materialization kernel**
that gathers every column type (incl. strings/nullables) in one typed pass. Small
win on Q1 (the memcpy is not its dominant cost — the row *volume* through the
shuffle is; see partial pre-aggregation below).

## Q1 (aggregate-heavy) — the real lever is partial pre-aggregation
Q1 shuffles ~60M rows to group into **4 groups**. The dominant costs are the
shuffle hashing/bucketing (`ScatterBuffered`) and the aggregate kernel, not the
materialization memcpy. The big win is **partial (local) pre-aggregation**:
aggregate within each scan/filter task (4 partial groups each), then shuffle only
the partials and combine. Removes the 60M-row shuffle almost entirely.

## Notes
- Remaining gap to hand-tuned C++ after gather: the PairBuffer round-trip
  (kernel pairs → `AddPair` copy → gather) and the per-slot value arrays in
  join_table.oz. Candidates if more is needed.
