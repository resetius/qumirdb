# Parallel Physical Execution Refactoring Plan

Goal: introduce parallel physical execution in qdb by first extracting the
validated scheduler pieces into `qdb/scheduler`, then lowering the whole
physical plan into a **single** task graph with explicit bounded connections,
executed by one scheduler (see the Single-Graph Invariant below). This mirrors
`test/test_scheduler.cpp`, not a tree of separately-scheduled runtime nodes.

The current toy implementation in `test/test_scheduler.cpp` has enough parts to
start the real refactoring:

- bounded SPSC rowset queues for one producer and one consumer;
- bounded MPMC ready queue for worker scheduling;
- explicit task states: `Idle`, `Queued`, `Running`, `Reschedule`, `Finished`;
- single-threaded scheduler;
- threaded scheduler that never executes the same task concurrently;
- partitioned graph with cloned partition-local state/params and resized
  connections.

This plan is a refactoring plan. Keep changes staged and commit complete pieces.
Do not mix planner/operator rewrites into queue/scheduler extraction commits.

## Current Work In Progress

Active approach — do not diverge from this without updating this note:

- We have switched to **single-graph lowering** (`TSchedulerGraphLowerer` /
  `BuildSchedulerPlanPipeline` in `qdb/scheduler/plan_lowerer.cpp`): every
  supported physical plan is recursively lowered into ONE `TTaskGraph`,
  executed by ONE scheduler, per the Single-Graph Invariant below.
- The planner's scheduler dispatch now calls `BuildSchedulerPlanPipeline(root)`;
  it returns null (→ serial fallback) only when the plan contains an operator
  the lowering does not yet support.
- Operator → graph shapes are as in the Single-Graph Invariant: scan/filter/
  project on `OneToOne` lanes; limit as `Gather` → single blocking task;
  sort as local sort lanes → `TMergeTask`; top-sort as local top-sort lanes →
  `TMergeTask` → final limit; equi-join as `HashShuffle` → partition-local join
  tasks; grouped aggregate as group-key `HashShuffle` → partition-local
  aggregate tasks; cross-scalar join as scalar gather/broadcast → local cross
  tasks. Current active work is scheduler stability and scalability, not adding
  more operator bridge layers.

### Cleanup Owed Before Finalizing The PR

- [x] Remove the superseded per-operator "bridge" builders once the single-graph
  lowering covers all cases: `TryBuildSchedulerUnaryPipeline`,
  `TryBuildSchedulerAggregatePipeline`, `TryBuildSchedulerLimitPipeline`,
  `TryBuildSchedulerSortPipeline`, `TryBuildSchedulerTopSortPipeline`,
  `TryBuildSchedulerJoinPipeline`, `BuildSchedulerUnarySpec`,
  `BuildSchedulerRuntimePipeline`, `TakePipelineSide`.
- [x] Move `TSchedulerGraphLowerer` out of `qdb/exec/planner.cpp`; planner now
  keeps scheduler dispatch plus serial runtime building, while scheduler graph
  wiring lives in `qdb/scheduler/plan_lowerer.cpp`.
- [x] Remove/retire the spec-based `TPipelinePartitioner` /
  `TJoinPipelinePartitioner` and `BuildBufferedScheduler*RuntimePipeline` if the
  lowering no longer uses them (keep only what a single graph needs).
- [x] Drop now-dead spec structs (`TPipelinePartitionSpec`,
  `TJoinPipelinePartitionSpec`, side/shuffle/binary specs) if unreferenced.
- [ ] General pass: remove inefficient or unused code introduced during this
  refactor; no dead scaffolding left in the PR.

## Target Layout

All scheduler-owned code lives under `qdb/scheduler`.

| Component | File | Purpose |
| --- | --- | --- |
| SPSC queue | `qdb/scheduler/spsc.h` | Bounded lock-free single-producer/single-consumer queue. Used by rowset channels where each logical lane has one producer and one consumer. |
| MPMC queue | `qdb/scheduler/mpmc.h` | Bounded lock-free multi-producer/multi-consumer queue. Used as the scheduler ready queue. |
| Task states | `qdb/scheduler/state.h` | Task execution state machine and scheduler return states. |
| Connections | `qdb/scheduler/connection.h/.cpp` | Explicit bounded data edges between task nodes. |
| Task graph | `qdb/scheduler/graph.h/.cpp` | DAG of executable task nodes and connection edges. |
| Plan lowerer | `qdb/scheduler/plan_lowerer.h/.cpp` | Recursively lowers a physical plan tree into ONE task graph, stitching operators with connections. Not one graph per operator. |
| Single-threaded scheduler | `qdb/scheduler/single_threaded_scheduler.h/.cpp` | Deterministic fallback scheduler for tests, debugging, and one-worker execution. |
| Threaded scheduler | `qdb/scheduler/threaded_scheduler.h/.cpp` | Fixed-size parallel scheduler using MPMC ready queue and atomic task states. |
| Runtime adapter | `qdb/scheduler/runtime_adapter.h/.cpp` | Bridges existing qdb runtime nodes to task-node execution. |
| Heuristic settings | `qdb/scheduler/settings.h` | Groups scan, shuffle, sort, aggregate, queue, and scheduler knobs used by the lowerer. |

Initial implementation may keep `.cpp` files small or header-only where the
template code requires it. Avoid speculative abstractions beyond these files.

Non-scheduler integration points:

- [ ] Parquet/source scan code must expose split descriptors that are smaller
  or more flexible than "one row group is one task" when the format allows it.
- [x] Kernel/codegen code must expose rowset-wide hash kernels for shuffle.
- [ ] Kernel/codegen code should expose compare kernels for merge/sort pipelines
  as an optimization. Current merge uses the existing C++ sort comparator and
  batched column gather path.
- [x] Existing compiled/JIT code artifacts must be shareable across partition
  tasks.

## Execution Model

### Single-Graph Invariant

This is the most important architectural invariant, on par with the code/data
split below.

The whole physical (sub)plan is lowered into **one** `TTaskGraph`, executed by
**one** scheduler, exactly like `test/test_scheduler.cpp`. There is no per-
operator sub-graph and no nested executor.

- [x] A supported physical plan lowers recursively into a single graph of task nodes and
  connections; operators are stitched together with `IConnection` edges, not
  serial `Next()` bridges between separate graphs.
- [x] Exactly one `TSchedulerExecutor` runs per query (serial fallback aside).
- [x] Operators map onto graph shapes:
  - inner equi-join: both inputs feed a `HashShuffle` connection into
    partition-local join tasks, then `Gather`;
  - sort: partition-local `sort` tasks feed a `TMergeTask`
    (`sort -> merge-task`);
  - top-sort: partition-local `top-sort` tasks feed `TMergeTask` then a final
    `limit` (`top-sort -> merge-task -> limit`);
  - grouped aggregate: `HashShuffle` by group key into partition-local
    aggregate tasks, then `Gather`;
  - cross-scalar join: scalar side is gathered and broadcast to vector lanes,
    then local cross tasks feed `Gather`;
  - scan/filter/project: `OneToOne` partition lanes.
- [ ] Any operator not yet lowerable stays behind a single serial gather node
  **inside** the same graph, never as its own scheduler run. Current behavior:
  unsupported plans fall back to the serial path as a whole.

Anti-pattern to remove: the `TRuntimeSchedulerPipeline` bridge that wraps each
operator group in its own graph and runs a private `TSchedulerExecutor` in
`Next()`. It was introduced in commit `8095a20` ("Add scheduler runtime node
bridge") and propagated through every subsequent scheduler operator path
(unary, aggregate, limit, sort, top-sort, join). It produces separate graphs
connected by serial `Next()` calls (and, once join inputs are wrapped, nested
executors). Replace it with the single-graph lowering above.

### Code/Data Split Invariant

Partitioning must split data and mutable execution state, not generated code.
This is the most important scheduler invariant.

- [ ] A physical operator has immutable code artifacts:
  - lowered expressions;
  - compiled/JIT kernels;
  - function pointers;
  - static operator descriptors.
- [ ] Partitioning must share those immutable code artifacts between tasks.
- [ ] Partitioning may clone only partition-local data:
  - scan range/cursor;
  - input/output ports;
  - runtime params;
  - mutable operator state;
  - aggregation hash tables/states;
  - join build/probe state;
  - sort/top-sort buffers;
  - per-task metrics.
- [ ] `Clone()`-style APIs must not duplicate JIT code or rebuild kernels.
- [ ] Runtime adapter APIs should make the split explicit: shared code bundle
  plus per-partition state object.
- [ ] Tests should catch accidental recompilation or code cloning when a plan is
  partitioned.

### Task Node

A task node is the scheduler unit. It wraps a runtime node or a small runtime
chain plus partition-local mutable state. The task may reference shared code, but
it owns only its state.

Required behavior:

- [ ] `Execute()` performs bounded work and returns one scheduler state.
- [ ] `NEED_DATA` means input queues have no rowset and the task should wake
  upstream producers.
- [ ] `BLOCKED_OUTPUT` means output queues are full and the task should wake
  downstream consumers.
- [ ] `OK` means useful work happened and the task may be scheduled again.
- [ ] `FINISHED` means the task has no more work and has signaled output finish.
- [ ] A task node must not be executed concurrently with itself.

### Task State Machine

The threaded scheduler owns an atomic state per task.

| State | Meaning |
| --- | --- |
| `Idle` | The task is not queued and not running. |
| `Queued` | The task is present in the MPMC ready queue. |
| `Running` | One worker is executing the task. |
| `Reschedule` | Another task tried to schedule this task while it was running. |
| `Finished` | The task completed permanently. |

Transitions:

- [ ] `Idle -> Queued`: successful schedule.
- [ ] `Queued -> Running`: worker pops task and claims execution.
- [ ] `Running -> Idle`: `Execute()` returned non-final state.
- [ ] `Running -> Finished`: `Execute()` returned `FINISHED`.
- [ ] `Running -> Reschedule`: another task schedules this running task.
- [ ] `Reschedule -> Idle -> Queued`: worker finishes current execution and
  requeues the task.
- [ ] Any schedule attempt against `Queued`, `Reschedule`, or `Finished` is a
  no-op.

This avoids lost wakeups without a mutex and preserves the SPSC assumption for
per-lane data queues.

### Connections

Connections are explicit physical edges between task nodes. They replace direct
parent-child `Next(TRowSet&)` calls across split boundaries.

| Kind | First implementation | Notes |
| --- | --- | --- |
| `OneToOne` | SPSC queue per `src_id == dst_id` lane | Main pipeline edge after partitioning. |
| `Gather` | SPSC queue per producer lane, one consumer | Consumer polls producer lanes round-robin. |
| `Merge` | `TMergeTask` + `TMergeProcessor` | Ordered N-input task for sort/top-sort. Not a connection: ordering needs sort keys, comparator, and merge state. |
| `HashShuffle` | Implemented for rowset scatter | Producer splits a rowset by hash key into destination lanes. |
| `Broadcast` | Implemented for scalar cross join | One scalar producer replicated to vector lanes. |
| `RoundRobin` | Later stage | Optional load distribution edge. |

Connection requirements:

- [ ] Bounded rowset capacity and backpressure.
- [ ] Move ownership of `TRowSet` across the edge.
- [ ] Generalize edge payloads from only `TRowSet` to a row-batch abstraction
  that can represent:
  - a compact/materialized `TRowSet`;
  - a single `TRowSet` view (`base + selection`);
  - a chunked rowset (`vector<base + selection>` fragments).
- [ ] Track producer finish independently per source lane.
- [ ] Return `OK`, `NO_DATA`, or `FINISHED` on fetch.
- [ ] Provide debug counters: pushed rowsets, popped rowsets, blocked pushes,
  empty fetches, max observed queue depth.

### Row Batch Representation

The scheduler should not force a choice between tiny selection-view rowsets and
eager materialization. Hash shuffle needs metadata coalescing first, and
physical materialization only when the downstream consumer benefits from it.

Target model:

```cpp
struct TRowSetView {
    std::shared_ptr<const TRowSet> Base;
    TSelectionVector Selection;
};

struct TChunkedRowSet {
    std::vector<TRowSetView> Views;
    size_t Rows = 0;
    size_t Bytes = 0;
};
```

The eventual edge payload can be a variant or a small interface:

```cpp
using TBatch = std::variant<TRowSet, TRowSetView, TChunkedRowSet>;
```

or:

```cpp
struct IRowBatch {
    virtual size_t NumRows() const = 0;
    virtual void ForEachFragment(...) const = 0;
};
```

The invariant is that JIT kernels still consume normal `TRowSet` fragments with
optional selection. The C++ adapter loops over fragments inside one logical
batch, so scheduler/connection overhead is paid once per chunked batch rather
than once per tiny shuffle fragment.

Ownership requirement:

- [ ] A fragment must own or retain its base rowset. Do not store borrowed
  references to stack/local `TRowSet` values.
- [ ] Prefer `shared_ptr`/intrusive ownership or an arena-backed batch handle;
  avoid copying `TRowSet` structs that duplicate the current by-value
  `RefCount`.
- [ ] Keep code/data split: chunking clones only metadata and selections, never
  JIT code or operator code bundles.

Consumer policy:

- [ ] Filter/project/probe-like one-pass consumers should support
  `ForEachFragment` and avoid materialization.
- [ ] Join build side and grouped aggregate input may request materialization
  because they repeatedly read/build state from the batch.
- [ ] Consumers that require contiguous columns call `Materialize()` explicitly.
- [ ] Hash shuffle accumulators choose flush mode by downstream capability and
  heuristics:
  - `ChunkedView` by default;
  - `Materialized` if the consumer requires contiguous input;
  - `Materialized` if fragment count is too high;
  - `Materialized` if selection metadata becomes too large relative to data;
  - `Materialized` for join build side or hash aggregate input if benchmarks
    show the dense layout wins.

Possible variant for the `Materialized` path — **single shared CSR-style output
buffer** instead of N per-partition buffers: radix-partition all buffered rows
into one contiguous buffer laid out in partition order (histogram → prefix-sum
offsets → one gather pass), then hand each destination lane a zero-copy slice
view `[off[p], off[p+1])` into that buffer. Wins vs the current per-partition
materialize: one allocation instead of N, a single cache-friendly gather pass,
and per-partition outputs become cheap slices. Complements ChunkedView (which
stays the default zero-copy path); CSR is only the strategy for when
materialization is actually required. Slicing needs care for var-length columns
(offset rebasing) and bit-packed null masks. Deferred — evaluate after
ChunkedView lands and only if a consumer genuinely needs contiguous input.

## Heuristic Settings

Parallelization must be controlled by grouped settings. Defaults should be
conservative; values will be tuned experimentally with TPC-H and microbenchmarks.

| Group | Example knobs | Purpose |
| --- | --- | --- |
| Scheduler | execution mode, worker count, max ready queue size | Select serial, single-threaded scheduler, or threaded scheduler. |
| Partitioning | default partition count, max partitions, min estimated rows/bytes to split | Avoid parallel overhead on tiny plans. |
| Queue | rowset capacity per lane, max queued bytes, debug counter enablement | Bound memory and backpressure. |
| Scan split | target rows/bytes per task, max scan tasks, min row groups per task, split strategy | Control parquet/source read parallelism. |
| Hash shuffle | partition count, max shuffle partitions, max producer lanes, target output batch rows/bytes | Bound join shuffle fanout and memory use. |
| Sort/top-sort | local partition count, merge fan-in, merge batch rows, local top limit multiplier | Control partitioned sort and top-sort shape. |
| Kernel helper | enable rowset hash kernel, enable compare kernel, scratch vector sizes | Control vectorized helper-kernel use. |

Settings are part of the plan/debug output:

- [ ] print effective worker count and partition counts;
- [ ] print scan split strategy and split sizes;
- [ ] print shuffle partition count and queue capacity;
- [ ] print sort/top-sort merge fan-in;
- [ ] print whether hash/compare helper kernels are used.

## Stage 0 - Preserve The Toy Prototype

- [ ] Keep `test/test_scheduler.cpp` as a prototype while extracting pieces.
- [ ] Add comments only where they describe prototype limitations that matter
  for production extraction.
- [ ] Do not make the test a production dependency.
- [ ] Keep the test green after every extraction step.

Verify:

- [ ] `ninja -C build test_scheduler`
- [ ] `build/test/test_scheduler`

Commit boundary:

- [ ] Prototype cleanups only, no production scheduler files.

## Stage 1 - Extract Queue Primitives

- [x] Add `qdb/scheduler/spsc.h` with bounded `TSPSC<T>`.
- [x] Add `qdb/scheduler/mpmc.h` with bounded `TMPMCQueue<T>`.
- [x] Keep both APIs minimal:
  - `TryPush`;
  - `TryPop`;
  - capacity in constructor;
  - no blocking waits;
  - no allocator/custom policy knobs.
- [x] Use atomics with explicit memory ordering.
- [x] Keep queue templates independent from qdb rowset types.
- [x] Add focused unit tests:
  - SPSC push/pop order;
  - SPSC full/empty behavior;
  - MPMC push/pop order for single producer;
  - MPMC no lost items with multiple producers/consumers;
  - bounded queue full behavior.

Verify:

- [x] Queue unit tests pass.
- [x] Existing scheduler prototype still passes.

Commit boundary:

- [ ] `qdb/scheduler/spsc.h`, `qdb/scheduler/mpmc.h`, queue tests only.

## Stage 2 - Add Scheduler Core Types

- [x] Add `qdb/scheduler/state.h`.
- [x] Add `qdb/scheduler/settings.h` with grouped settings structs.
- [x] Define scheduler execution result states:
  - `OK`;
  - `NEED_DATA`;
  - `BLOCKED_OUTPUT`;
  - `FINISHED`.
- [x] Define task state enum:
  - `Idle`;
  - `Queued`;
  - `Running`;
  - `Reschedule`;
  - `Finished`.
- [x] Add a small task-node interface for scheduler-owned code.
- [x] Add settings groups without wiring behavior yet:
  - scheduler;
  - partitioning;
  - queues;
  - scan split;
  - hash shuffle;
  - sort/top-sort;
  - kernel helpers.
- [x] Keep this separate from qdb physical operators for now.

Verify:

- [x] Unit tests compile without pulling physical planner headers.

Commit boundary:

- [ ] scheduler core types and small tests only.

## Stage 3 - Production-Shaped Connections

- [x] Add `qdb/scheduler/connection.h/.cpp`.
- [x] Implement `OneToOne` over `TSPSC<TRowSet>`.
- [x] Implement `Gather` over one `TSPSC<TRowSet>` per producer lane.
- [x] Keep `Merge`, `HashShuffle`, `Broadcast`, and `RoundRobin` declared only
  if needed by graph shape; otherwise leave them out until their stage.
- [x] Make lane counts explicit with `Resize(srcCount, dstCount)` or constructor
  parameters.
- [x] Ensure `CanPush`, `Push`, `Finish`, and `Fetch` match the prototype
  semantics.
- [x] Add rowset ownership tests.
- [x] Add finish propagation tests.
- [x] Add backpressure tests.

Verify:

- [x] Connection tests pass.
- [x] No direct dependency on SQL/planner code.

Commit boundary:

- [ ] connection implementation and connection tests only.

## Stage 4 - Task Graph And Single-Threaded Scheduler

- [x] Add `qdb/scheduler/graph.h/.cpp`.
- [x] Represent nodes, inbound/outbound edges, and owned connections.
- [x] Store edge-to-connection mapping explicitly.
- [x] Add `qdb/scheduler/single_threaded_scheduler.h/.cpp`.
- [x] Port the prototype single-threaded scheduler behavior.
- [x] Keep it available permanently as:
  - serial fallback;
  - deterministic debug path;
  - one-worker compatibility check.
- [x] Add graph build validation:
  - root exists when required;
  - no missing connection for an edge;
  - lane ids are valid;
  - finished graph cannot be run twice without reset unless explicitly allowed.

Verify:

- [x] Graph/single-threaded scheduler unit tests pass.
- [x] Prototype test can be reduced or switched to production scheduler pieces.

Commit boundary:

- [ ] graph plus single-threaded scheduler and tests.

## Stage 5 - Threaded Scheduler

- [x] Add `qdb/scheduler/threaded_scheduler.h/.cpp`.
- [x] Use `TMPMCQueue<TTaskNode*>` as the ready queue.
- [x] Use per-task atomic state machine from Stage 2.
- [x] Guarantee at most one worker executes a task at a time.
- [x] Preserve `Reschedule` behavior for schedule attempts during execution.
- [x] Start with `std::thread` workers and simple `yield` when ready queue is
  empty.
- [ ] Add cancellation/error propagation after basic correctness is stable.
- [x] Add fixed thread count option at construction time.
- [x] Do not implement work stealing yet.

Verify:

- [x] Threaded scheduler unit test with a small split graph.
- [x] Stress test with repeated producer/consumer wakeups.
- [x] Threaded scheduler with one worker matches single-threaded scheduler.

Commit boundary:

- [ ] threaded scheduler and scheduler tests only.

## Stage 6 - Runtime Adapter

- [x] Add `qdb/scheduler/runtime_adapter.h/.cpp`.
- [x] Wrap existing qdb runtime nodes into scheduler task nodes.
- [x] Preserve current serial `IRuntimeNode::Next(TRowSet&)` execution path.
- [x] Do not rewrite all runtime nodes at once.
- [x] Represent adapted operators as:
  - shared immutable code bundle;
  - partition-local mutable state;
  - partition-local params/ranges/ports.
- [x] Ensure partitioning never recompiles or clones JIT code.
- [x] First adapter can support only pipeline-compatible unary nodes:
  - scan/source;
  - filter;
  - project;
  - final sink.
- [x] Add explicit input and output ports to adapter state.
- [x] Keep mutable runtime state partition-local.
- [x] Make shared JIT kernels lifetime-safe for all partition tasks.
- [ ] Add debug counters for code bundle reuse vs accidental recompilation.

Verify:

- [x] Serial execution path remains green.
- [x] Adapter tests can execute scan/filter/project through scheduler graph.
- [x] Adapter tests prove that partitioning clones state/params, not compiled
  code.
- [x] Blocking tails can share compiled code and own only mutable state:
  aggregate, limit, sort, and top-sort all use reusable processor objects.

Commit boundary:

- [ ] adapter and minimal adapter tests only.

## Stage 7 - Single-Graph Plan Lowerer

- [x] Add `qdb/scheduler/plan_lowerer.h/.cpp`.
- [x] Start from the physical operator tree, not from a serial runtime tree.
- [x] Lower the whole supported physical plan into one `TTaskGraph`.
- [x] Split code descriptors from data/state descriptors before creating
  partition-local tasks.
- [x] First heuristics:
  - [x] source leaves get `N` scan lanes;
  - [x] `OneToOne` keeps lane count;
  - [x] `Gather` collapses to one consumer for blocking tails and final output;
  - [ ] `Merge` collapses to one ordered consumer;
  - [ ] unsupported operators stay as explicit serial nodes inside the same graph.
- [x] Clone or create only partition-local runtime state for each task.
- [x] Share code bundles and kernel handles across all partitions of the same
  original operator.
- [x] Resize connections based on source and destination lane counts.
- [x] Read partitioning knobs from `qdb/scheduler/settings.h`.
- [ ] Add/restore debug printer for:
  - [x] nodes;
  - [x] partition counts;
  - [ ] shared code bundle ids;
  - [x] edges;
  - [x] connection kinds;
  - [x] lane mapping;
  - [ ] heuristic decisions and settings used.

Verify:

- [x] Partitioner unit tests for `scan -> filter -> gather -> sink`.
- [x] Partitioned graph with one partition behaves like serial graph.
- [x] Partitioned graph with multiple partitions matches serial results where
  order is not required.

Commit boundary:

- [x] single-graph lowerer and graph cleanup.
- [ ] graph debug output.

## Stage 8 - Executor Integration

- [x] Add runtime option to select physical execution mode:
  - [x] current serial path;
  - [x] scheduler single-threaded path;
  - [x] scheduler threaded path.
- [x] Add thread count option.
- [x] Default to current serial path until result parity is demonstrated.
- [ ] Make one-worker scheduler mode part of CI before enabling multi-worker
  mode broadly.
- [ ] Ensure errors cancel the graph and finish worker threads.
- [ ] Ensure all retained rowsets are released on cancellation.

Verify:

- [ ] Existing tests pass in current serial mode.
- [x] Selected small scheduler graph tests pass in scheduler single-threaded mode.
- [x] Selected small scheduler graph tests pass in scheduler threaded mode.

Commit boundary:

- [ ] executor switch and minimal integration tests.

## Stage 9 - Parallel Scans And Unary Pipelines

- [ ] Define scan split descriptors before implementing scheduling:
  - source object;
  - column set;
  - predicate/pushdown descriptor;
  - row group range;
  - optional row/page/batch range inside a row group;
  - target row/byte budget.
- [ ] Do not assume "one parquet row group is one task"; this may be too
  expensive or too coarse.
- [ ] Investigate practical parquet split units:
  - multiple row groups per task;
  - row group fragments by page/range if metadata and reader support it;
  - decoded rowset batches pulled from one shared reader only if thread-safe;
  - source-specific fallback to serial read plus parallel downstream pipeline.
- [ ] Use scan split settings:
  - target rows/bytes per task;
  - min split size;
  - max scan tasks;
  - row group coalescing factor;
  - tiny-input serial threshold.
- [ ] Run scan partitions through local filter/project.
- [x] Gather into scheduler blocking tails for aggregate, limit, sort, and
  top-sort.
- [ ] Keep unsupported operators behind the old serial executor until their
  scheduler task shape is explicit.
- [ ] Keep tiny inputs serial to avoid overhead.
- [ ] Preserve column pruning and predicate pushdown per split.

Verify:

- [ ] Scan/filter/project result parity.
- [ ] TPC-H scan-heavy queries such as Q1/Q6 behave correctly.
- [ ] Compare single-threaded scheduler, threaded scheduler, and old serial path.

Commit boundary:

- [ ] first end-to-end parallel execution path for scans/unary pipelines.

## Stage 10 - Hash Shuffle And Join

- [x] Implement `HashShuffle` connection.
- [x] Add shuffle settings:
  - partition count;
  - max partition count;
  - max producer lanes;
  - max queued rowsets/bytes per destination lane;
  - target output batch rows/bytes;
  - serial fallback threshold.
- [x] Expose a rowset-wide hash kernel for join keys.
- [x] Extract the hash helper from the following join kernel shape:
  - [x] same key expressions and null/hash semantics as the join kernel;
  - [x] input is one `TRowSet`;
  - [x] output is a caller-owned `uint64_t` vector with one hash per physical
    row;
  - [x] selection rows are not applied by the helper; scatter skips unselected
    rows.
- [ ] Expose a rowset-wide hash kernel for aggregate group keys when aggregate
  shuffle starts.
- [x] Hash shuffle task path:
  - [x] run the hash kernel once per input rowset;
  - [x] fill a scratch vector with hash values for all physical rows;
  - [x] skip unselected rows in the scatter step;
  - [x] split/scatter rows by partition using the vector;
  - [x] move partition rowsets into destination SPSC lanes.
- [x] Hash shuffle graph wiring:
  - [x] build shuffle tasks for both join inputs;
  - [x] connect shuffle outputs to partition-local binary tasks;
  - [x] gather partition-local outputs;
  - [x] preserve the code/data split: hash and binary task code are shared code
    bundles, only task state/ports/scratch buffers are per partition.
- [x] Planner/runtime join wiring:
  - recognize scheduler-compatible left and right input pipelines;
  - compile join kernels once;
  - expose left/right join hash helpers as shared `THashShuffleCode`;
  - create partition-local join state using shared join kernels;
  - return a scheduler runtime node for supported equi-joins.
- [ ] Replace temporary hash-shuffle output compaction with chunked row-batch
  coalescing:
  - per destination, accumulate `TRowSetView` fragments (`base rowset +
    selection`) instead of immediately pushing one tiny output per destination;
  - flush one destination at a time when target rows/bytes is reached, not all
    destinations at once;
  - flush all remaining destinations on producer finish;
  - default flush mode should be `ChunkedView`, not physical materialization;
  - keep the current materialized path only as an explicit strategy for
    consumers that ask for contiguous input or for heuristics that prove it is
    cheaper.
- [ ] Add a `TPartitionAccumulator` helper for hash shuffle:
  - owns fragment base rowsets safely;
  - tracks row count, estimated bytes, fragment count, and selection bytes;
  - supports `Ready()`, `FlushChunkedView()`, and `FlushMaterialized()`;
  - keeps target rows (`16K`), max rows (`64K`), and target bytes (`1MB`) as
    settings, not constants in the scatter loop.
- [ ] Extend scheduler connections/ports to move a row-batch payload rather than
  only `TRowSet`.
- [ ] Add consumer capability bits or explicit lowering decisions:
  - probe side join can process chunked fragments without copy;
  - filter/project can process chunked fragments without copy;
  - join build side may request materialization;
  - grouped aggregate may request materialization.
- [ ] Add `VisitFragments(batch, fn)` helper so existing kernels can still be
  called on ordinary `TRowSet + optional selection` fragments.
- [ ] Hash helper contract:
  - input is the same rowset shape the following join kernel consumes;
  - null/hash semantics match join probing;
  - helper writes one hash per physical row;
  - scatter owns selection filtering and `hash % Partitions` routing.
- [x] First implementation scatters in C++ after vectorized hash calculation.
- [ ] Later implementation may move scatter into a kernel if profiling shows it
  matters.
- [x] Reuse existing key hashing/null semantics.
- [x] Add a scheduler graph builder for split equi-join shape.
- [x] Split real equi-join inputs by join keys in planner integration.
- [x] Run real partition-local join tasks.
- [x] Gather partition outputs in the graph-level join pipeline.
- [x] Start with inner join.
- [x] Add semi/anti/outer joins only after inner join is stable. Hash-
  partitioning co-locates matches, so partition-local outer/semi/anti joins are
  globally correct. `TInnerJoinProcessor` takes `EJoinType` + `hasResidual`:
  - Left/Right outer: both sides materialized, then `FinalizeOuter` emits
    unmatched rows (TPC-H Q13, Q17).
  - LeftSemi/LeftAnti: blocking — consume all left (build left table + rows),
    then all right (`InsertKeyOnly`), then `FinalizeAntiSemi`; residual variant
    collects matched left ids and emits by membership (TPC-H Q4, Q20, Q21, Q22
    all match serial).
  The planner allows Inner/Left/Right/LeftSemi/LeftAnti.
- [x] Cross join heuristic: a cross join with a **scalar** (single-row) side is
  parallelizable by broadcasting the scalar to every partition of the vector
  side and cross-joining locally. Common in decorrelated scalar subqueries
  (e.g. TPC-H Q22 `c_acctbal > (select avg ...)`).
  - Shape: `vector[N] x Broadcast(scalar) -> local cross[N] -> Gather`; the
    scalar side is a small (aggregate) subtree materialized once and broadcast.
  - **cross + residual = cross + filter**: the join's residual predicate is
    just a normal filter over the glued (vector + scalar) schema, applied per
    partition after the local cross. No join-residual kernel needed.
  - Pieces added: `TBroadcastConnection` (1 src -> N dst, every rowset
    replicated), cross-join processor (buffer the broadcast side, stream the
    vector side), scalar-side detection (subtree terminating in an aggregate).
  - **Prerequisite bug (FIXED for serial)**: the serial empty-keys path
    (`TRuntimeCrossJoin`) dropped the join's residual filter (measured:
    `c_acctbal > avg` returned 150000 instead of the correct filtered count).
    `TPhysicalPlanner::Build` now wraps a keyless join carrying a residual in a
    filter over the glued output schema (`cross -> filter`); TPC-H Q22 now
    applies the residual. Serial + all tests green.
  - **Scheduler path**: `vector[N] x Broadcast(scalar) -> local cross[N] ->
    filter(residual) -> Gather` is implemented for the scalar-on-right shape.
- [ ] Add partition size metrics for skew visibility.

The first scatter implementation forwards selection rowsets over shared input
ownership. It does not copy columns and it does not clone compiled code. This is
good enough for pipeline integration and correctness tests; compaction is a
separate profiling-driven optimization.

Old checklist retained as design constraints:

- [x] run the hash kernel once per input rowset;
- [x] fill a scratch vector with hash values or partition ids for all selected
    rows;
- [x] split/scatter rows by partition using the vector;
- [x] move partition rowsets into destination SPSC lanes.

Verify:

- [ ] Hash partition correctness tests.
- [ ] Inner equi-join result parity.
- [ ] TPC-H join-heavy queries validated incrementally.

## Stage 11 - Top-Sort, Sort, And Aggregation

Top-sort:

- [x] Add scheduler blocking top-sort tail after parallel scan/filter/project.
- [x] Share top-sort code descriptors and clone only mutable top-sort state.
- [x] Add local top-sort per partition (each keeps its local top-K, limit = the
  top-sort limit; no offset on the top-sort operator).
- [x] Rewrite as `top-sort over partitions -> merge -> limit`. Reuses
  `BuildSortMerge` with the top-sort local task, then a single `limit` task
  reads the merged stream. Falls back to the single gathered top-sort when the
  input has one lane.
- [x] Add an explicit merge node between local top-sort outputs and final
  limit (`TMergeTask`).
- [x] Use the same comparator/gather path as partitioned sort
  (`SortRowsLessColumns` + `GatherColumn`).
- [x] Keep the final limit node serial first (single `limit` task after merge).
- [~] Preserve current tie behavior or document the difference. Order among
  equal keys is unspecified (heap picks arbitrarily among ties), consistent
  with the non-stable radix sort; verified value parity serial == single on a
  `order by ... limit` query.

Sort:

- [x] Add scheduler blocking sort tail after parallel scan/filter/project.
- [x] Share sort code descriptors and clone only mutable sort state.
- [x] Add local sort per partition.
- [x] Rewrite as `partitioned sort -> merge` (only when the sort input has >1
  lane; single-lane input keeps the gathered single sort).
- [x] Add ordered merge connection/operator: `TMergeTask` (N-input blocking
  task, `qdb/scheduler/runtime_adapter.*`) + `TMergeProcessor`
  (`qdb/exec/sort_exec.*`). N local `sort` tasks each write a sorted run over a
  `OneToOne(1)` edge; the merge task reads all runs and emits one stream.
- [~] Expose compare kernels used by sort order expressions. Reused the existing
  C++ row comparator `SortRowsLess`/`SortRowsLessColumns` (already used by the
  sort's stable-sort fallback) for the k-way heap. A JIT compare kernel is a
  later optimization.
- [x] Merge must work in rowset/batch terms, not one cell at a time. The k-way
  merge orders whole runs via `TRowId` (batch, row) cursors and materializes
  output with the batched `GatherColumn`; only the heap heads are compared.
- [x] Chosen merge algorithm: **k-way heap over run cursors** (min-heap of run
  heads, comparator = `SortRowsLessColumns`).
- [ ] Add merge fan-in and merge batch size settings.
- [ ] Follow-up: current merge is a **buffering** merge — the merge task holds
  all runs in a row store (O(n) memory) before emitting. Make it **streaming**
  (bounded memory, pinning only the live batch per run) as an optimization.
- [x] `top-sort -> merge -> limit` is implemented for multi-lane input. Single
  lane input still uses the simpler gathered top-sort.

Aggregate:

- [x] Add scheduler blocking aggregate tail after parallel scan/filter/project.
- [x] Share aggregate kernels and clone only mutable aggregate hash table state.
- [x] Add local aggregate (partition-local aggregate task per shuffle lane).
- [x] Add final aggregate merge for the opt-in ungrouped/global cascade path.
  Grouped hash-partitioned aggregate does not need a final merge: disjoint
  groups per partition are gathered directly.
- [x] Start with ungrouped aggregate. Ungrouped aggregate has no group key to
  shuffle on; default path still stays a single gathered aggregate because that
  is faster for cheap aggregates.
- [x] Parallelize ungrouped aggregate via a **one-level cascade**: partition-
  local partial aggregate -> `Gather` -> final combining aggregate. Combine
  rule per function: `count -> gather -> sum(counts)`, `sum -> sum`,
  `min/max -> min/max`, `avg` finalized from partial `sum` + `count`. This
  removes the single-node bottleneck for ungrouped aggregates (Q1-style scalar
  sums, and the scalar sides of cross/scalar-subquery joins). This is opt-in
  via `--cascade-aggregates`.
- [x] Add grouped aggregate with hash partition by group key:
  `child[N] -> HashShuffle(by group key)[N->M] -> local-aggregate[M] -> Gather`.
  Matching keys land in one partition, so each partition computes complete
  groups. `OutputLanes(grouped aggregate) = M`; parent gathers.
- [x] Group-key hash: a C++ `MakeGroupKeyHash` over the group-key columns
  (`plan_lowerer.cpp`) feeds the shuffle. The shuffle only needs a consistent
  hash (equal keys -> equal partition), not the aggregate kernel's internal
  hash, so this handles all key types including strings without JIT. A JIT
  group-key hash (`TKernelCompiler::CompileGroupHash`) exists for a future fast
  path but is not yet wired (string keys in JIT are the follow-up).
- [~] Decide floating-point tolerance for changed merge order. Grouped-aggregate
  float sums differ from serial only by parallel summation order; measured max
  relative diff on TPC-H Q1 is ~7e-14 (integer counts/sums match exactly).

Verify:

- [ ] Query-specific result parity tests.
- [ ] TPC-H coverage for Q1/Q3/Q5/Q7/Q9/Q10/Q18/Q21 as each operator class is
  enabled.

## Stage 12 - Scheduler Stabilization And Scaling

Do this before adding more operator shapes. The goal is to make the threaded
scheduler trustworthy for already-lowerable graphs.

Known repros:

- Q15 result loss with higher thread counts:
  `MODE=sexp QDB_ARGS='--scheduler threaded --scheduler-workers 18 --scan-tasks 18' ../benchmark/tpch/run_tpch.sh ~/Projects/tpch 1 15`
  finishes but returns no rows. This is not a rowset-drop bug in the scheduler:
  Q15 compares `total_revenue == max(total_revenue)`, where `total_revenue` is
  `sum(l_extendedprice)`. TPC-H expects exact DECIMAL arithmetic for this path;
  qdb currently lowers it to `f64`, so two independently computed sums may
  differ by summation order and fail exact equality. Correct SQL semantics need
  DECIMAL support (or an equivalent exact fixed-point representation), not a
  scheduler workaround.
- Full naked-plan run at SF10:
  `QDB_ARGS='--scheduler threaded --scheduler-workers 6 --scan-tasks 18' ../benchmark/tpch/run_tpch.sh ~/Projects/tpch 10`
  should match serial/single results query by query.
- Q9 becomes very slow at high worker counts. Treat this as a scalability bug,
  not as an operator feature request. Root cause found: shuffle partition count
  was implicitly tied to `--scheduler-workers`, so Q9's chain of equi-joins and
  grouped aggregate created excessive hash-shuffle fanout when workers were
  raised. Keep worker count and shuffle partition count independent.

Assumptions to verify, not assume:

- [ ] `threaded --scheduler-workers 1` matches `single` for every tested plan.
- [ ] `threaded` with N workers and one scan task matches `single`.
- [ ] increasing only `--scan-tasks` does not change results for queries whose
  expected semantics are supported by qdb's current numeric types. Q15 is
  excluded until DECIMAL/fixed-point arithmetic exists.
- [ ] increasing only `--shuffle-partitions` does not change results.
- [ ] bounded queues never drop a rowset after observing producer finish.
- [ ] every task that returns `NEED_DATA`/`BLOCKED_OUTPUT` has a reliable
  wakeup path from the producer/consumer that can make progress.
- [ ] scheduler termination cannot complete while any reachable output rowset is
  still buffered in a connection.

Instrumentation first:

- [x] Add optional per-connection counters gated by settings:
  pushed rowsets, popped rowsets, finishes, blocked pushes, empty fetches,
  finished fetches. Max queue depth is not included yet because `TSPSC` does
  not currently expose depth.
- [x] Add scheduler counters:
  scheduled, popped, executed, `NEED_DATA`, `BLOCKED_OUTPUT`, `OK`,
  `FINISHED`, failed ready-queue push retries, reschedules.
- [x] Print counters in diagnostics for `--verbose --scheduler-counters`.
- [ ] Add a compact per-query summary for TPC-H comparisons:
  scheduler mode, workers, scan tasks, shuffle partitions, output rowsets,
  output rows, elapsed time.

Correctness work:

- [ ] Build a deterministic stress test for many producers -> hash shuffle ->
  many consumers where producers finish while consumers are repeatedly
  scheduled.
- [ ] Build a deterministic stress test for gather/broadcast finish replay.
- [ ] Make connection finish checks per-lane and queue-aware; do not rely only
  on aggregate finished counters when deciding `FINISHED`.
- [ ] Add DECIMAL/fixed-point arithmetic before treating Q15 as a result-parity
  acceptance query. Without exact revenue sums, `sum(f64) == max(sum(f64))`
  is order-sensitive and cannot be made SQL-correct by scheduler changes.
- [ ] After DECIMAL support, verify Q15 matrix:
  workers `{1,2,4,6,7,8,12,18}` x scan tasks `{1,6,18}`.
- [ ] After DECIMAL support, verify all scheduler modes for Q15:
  serial, single, threaded-1, threaded-N.

Performance work:

- [x] Profile Q9 with high workers and identify whether time is spent in:
  ready queue contention, rowset shuffle fanout, join build/probe, aggregate,
  merge/sort, or parquet scan.
- [x] Use `--shuffle-partitions` and `--shuffle-queue` to separate scheduler
  contention from operator work.
- [x] Add fanout guardrails: cap shuffle partitions independently from worker
  count when rowsets are small or skewed.
- [ ] Replace eager hash-shuffle materialization with chunked metadata
  coalescing once row-batch payloads exist. Current materialized batching is a
  mitigation for wrapper fanout, not the final design.
- [ ] Measure three shuffle output strategies on Q9/SF100 before making a
  default permanent:
  - selection-view without coalescing;
  - chunked view coalescing;
  - physical materialization.

Acceptance:

- [ ] Q15 returns the same result as serial for worker counts above 6, after
  DECIMAL/fixed-point support makes Q15 numerically well-defined.
- [x] Q9 does not regress materially as worker count increases; if it does, the
  bottleneck is measured and documented with counters.
- [ ] Full naked-plan SF10 run with
  `--scheduler threaded --scheduler-workers 6 --scan-tasks 18` completes with
  result parity against serial/single for every supported query.

## Testing Matrix

- [ ] Queue unit tests.
- [ ] Connection unit tests.
- [ ] Scheduler graph unit tests.
- [ ] Single-threaded scheduler tests.
- [ ] Threaded scheduler stress tests.
- [ ] Runtime adapter tests.
- [ ] Code/data split tests: partitioning must not clone or recompile code
  artifacts.
- [ ] Heuristic settings tests: effective settings are applied and printed.
- [ ] Partitioner tests.
- [ ] Parquet split descriptor tests.
- [ ] Hash helper kernel tests over whole rowsets.
- [ ] Compare helper kernel tests for sort/merge ordering.
- [ ] Merge node tests with rowset/batch cursors.
- [ ] SQL end-to-end tests with scheduler disabled.
- [ ] SQL end-to-end tests with scheduler single-threaded.
- [ ] SQL end-to-end tests with scheduler threaded.
- [ ] TPC-H SF1 serial-vs-parallel result comparison.

## Risks And Guardrails

- [ ] Accidental code cloning or JIT recompilation during partitioning.
  Guardrail: explicit shared code bundle plus partition-local state API and
  tests that count code artifacts.
- [ ] Lost wakeups between producers and consumers.
  Guardrail: task `Reschedule` state and stress tests.
- [ ] Accidental concurrent execution of one runtime node.
  Guardrail: per-task atomic state and debug assertions.
- [ ] Breaking SPSC assumptions by sharing one lane between producers.
  Guardrail: lane mapping validation in graph build.
- [ ] Deadlocks from bounded queues.
  Guardrail: nonblocking `Execute()` states and scheduler wakeups.
- [ ] Memory blowups in shuffle.
  Guardrail: bounded queues, shuffle settings, and rowset counters.
- [ ] Hash shuffle fanout creates too many tiny rowsets.
  Guardrail: target output batch settings and serial fallback thresholds.
- [ ] Parquet row-group-per-task split is too coarse or too expensive.
  Guardrail: scan split settings and explicit investigation of page/range/batch
  split options.
- [ ] Merge degenerates into per-cell interpreted comparison.
  Guardrail: expose compare kernels and design merge around rowset/batch
  cursors.
- [ ] Nondeterministic output order breaking tests.
  Guardrail: only unordered queries may gather arbitrarily; ordered queries need
  final sort/top-sort/merge.
- [ ] Floating-point aggregate differences.
  Guardrail: deterministic mode or explicit tolerance.
- [ ] Parallel scheduler hiding JIT compile races.
  Guardrail: compile kernels before graph execution and keep shared code
  artifacts immutable/lifetime-safe.

## Non-Goals For First Production Pass

- [ ] Distributed execution.
- [ ] Spill to disk.
- [ ] Adaptive repartitioning.
- [ ] Work stealing.
- [ ] Parallel CSV/console output.
- [ ] Parallel non-equi joins.
- [ ] New join algorithms beyond partitioned current equi join.
- [ ] Replacing the existing serial executor.

## Acceptance Criteria

- [ ] Current serial path remains available and green.
- [ ] Partitioning shares compiled/JIT code and clones only partition-local
  state/params.
- [ ] Effective heuristic settings are visible in debug output.
- [ ] Single-threaded scheduler path can run selected physical plans.
- [ ] Threaded scheduler with one worker matches single-threaded scheduler.
- [ ] Threaded scheduler with multiple workers matches serial results for
  unordered query outputs.
- [ ] Ordered queries remain deterministic through explicit final ordering.
- [ ] Scan-heavy queries show speedup after Stage 9.
- [ ] Join-heavy queries show speedup after Stage 10.
- [ ] Hash shuffle uses rowset-wide hash calculation before row scattering.
- [ ] Partitioned sort/top-sort uses explicit merge nodes.
- [ ] Debug output is sufficient to inspect graph shape, partition counts,
  shared code bundles, connection kinds, lane mapping, and heuristic decisions.

## Known Issues / Investigation Notes

### Q15 empty result at high worker counts

Status: blocked on numeric type support, not on scheduler mechanics.

Q15's SQL shape is:

- compute `revenue0` as grouped `sum(l_extendedprice)`;
- compute `max(total_revenue)` from the same logical `revenue0`;
- return suppliers where `total_revenue = max(total_revenue)`.

In qdb's current sexpr plan this becomes two independent grouped aggregates
over `f64`, followed by a keyless join and a filter:
`total_revenue == max_revenue`. TPC-H's intended semantics for prices are
exact DECIMAL/fixed-point arithmetic. qdb currently represents these values as
`f64`, so parallel execution can change summation order between the two
independent branches. The two mathematically equal revenues may differ in the
lowest bits, and exact `==` filters out every row.

Counters ruled out the earlier dropped-rowset hypothesis: in failing runs the
join/shuffle connections drain normally, and the result disappears at the
`f64` equality filter. Making gather deterministic would only hide the symptom
by serializing source lanes and breaking parallelism; it would not restore SQL
DECIMAL semantics.

Required fix:

- [ ] Add DECIMAL/fixed-point type support for TPC-H money columns and
  arithmetic.
- [ ] Make `sum`, `max`, equality, hash keys, sort keys, and output formatting
  handle DECIMAL.
- [ ] Revisit Q15 after DECIMAL support; only then use it as a scheduler
  result-parity acceptance query.

### Q9 high-worker slowdown

Status: diagnosed and partially mitigated.

Q9 does not contain `f64 ==` on aggregate output. It computes `amount` as `f64`
and then performs `sum(amount)`, so it can have small floating-point aggregate
differences but should not lose rows through exact equality.

The observed slowdown at high worker counts was caused by shuffle oversharding:
the default shuffle partition count was tied to `--scheduler-workers`. Q9 has
a chain of equi-joins plus a grouped aggregate, so raising workers from 6 to 18
also multiplied hash-shuffle fanout across many stages. On SF1 this changed
Q9 from roughly `0.49s` at 6 workers to roughly `4.0s` at 18 workers. Explicit
`--shuffle-partitions 6` with 18 workers brought it back to roughly `0.56s`.

Current mitigation:

- [x] `HashShuffle.PartitionCount == 0` means auto.
- [x] Auto shuffle partitions are derived from scan tasks, capped by workers,
  instead of being equal to workers.
- [x] Explicit `--shuffle-partitions 1` now means one partition instead of
  being treated as auto.

Remaining work:

- [ ] Add debug output for effective shuffle partition count, not only the raw
  configured value.
- [ ] Add hash-shuffle output coalescing without mandatory materialization.
  Current shuffle can split one input rowset into many tiny destination rowsets;
  after several join stages Q9 spends too much time on per-rowset overhead.
  The final fix should batch metadata per destination into `ChunkedRowSet`
  payloads up to target row/byte thresholds before pushing downstream. Physical
  materialization should happen only when a consumer requires contiguous input
  or a measured heuristic says the dense layout wins.
- [ ] Add size/cardinality-aware heuristics so tiny or skewed stages do not get
  unnecessary shuffle fanout.
- [ ] Avoid repartitioning when an upstream partitioning is already compatible
  with the next join/group key.
