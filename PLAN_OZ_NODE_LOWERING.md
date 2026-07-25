# Oz Node Lowering Plan

Goal: reduce per-operator C++ machinery by lowering compute behavior into
generated oz-lang kernels and shared oz source modules. C++ should become a
small executor shell around a few generic runtime node kinds, while operator
specific logic lives in oz.

This is a planning document only. The migration should be incremental and keep
existing query behavior green at every stage.

## Current Status

Implemented through Stage 5 (Join). Filter/project, aggregate, sort/top-sort,
and join now use the kernel-spec boundary and generic runtime shells where
practical. Join lowering is complete for the current algorithmic scope:
hash/probe/update/residual logic lives in oz kernels/source modules, while C++
keeps rowset/state ownership and output publication.

Validated after Stage 5:

- existing CTest unit suite: 25/25 passed;
- TPC-H SF1 Q21 in SQL mode: passed.

Stage 6 (Delim Join Readiness) is intentionally deferred until
`PLAN_DELIM_JOIN.md` becomes active.

## Motivation

The current runtime works well, but operator implementations are drifting toward
large custom C++ nodes. Sort, top-sort, join, aggregate, project, and filter all
own pieces of:

- row access;
- null/mask semantics;
- rowset ownership;
- output materialization;
- key extraction;
- comparison;
- hash state update/probe;
- algorithm-specific scratch buffers.

This makes every new operator more expensive than it should be. The target is a
small set of generic runtime shells plus oz kernels generated from plan-specific
descriptors.

New join algorithms are not a goal of this plan. The join-related work here is
only about moving existing join behavior into the same shell/kernel-spec model
as other compute nodes. New algorithms such as grace/spill joins, merge joins,
mark joins, or delim joins are low priority for this plan and are not required
to achieve the C++ reduction/unification goal.

The design should stay compatible with future parallel physical execution; see
`PLAN_PARALLEL_PHYSICAL_EXECUTION.md`. The lowering work is not expected to make
parallelization harder, but runtime shells and generated kernels should avoid
hidden assumptions that only hold in a single-threaded pull tree.

## Target Operator Kinds

- [ ] `non-compute`: schema/view-only transformations.
  - zero-copy project/rename;
  - cheap limit/selection views if possible;
  - no compiled kernel unless a predicate/expression must run.
- [ ] `unary streaming compute`: one input batch produces zero or one output
  batch.
  - filter;
  - materializing project;
  - simple expression evaluation.
- [ ] `unary blocking compute`: one input stream is consumed before output.
  - aggregate;
  - sort;
  - top-sort;
  - future distinct/window-like operators.
- [ ] `binary compute`: two input streams with shared state.
  - join;
  - future delim join / mark join.

For nodes of the same kind, C++ control flow should be identical.

## Target Architecture

```text
Logical plan
  -> physical planner
  -> TOperatorKernelSpec
  -> oz AST/module generation
  -> compiled kernel bundle
  -> generic runtime shell
```

C++ responsibilities:

- [ ] choose the generic runtime shell;
- [ ] build a typed kernel spec from the logical operator;
- [ ] generate or compose oz AST for entrypoints;
- [ ] compile and cache kernels;
- [ ] own rowset/state lifetimes;
- [ ] call entrypoints through stable ABI;
- [ ] expose `TRowSet` output.

Oz responsibilities:

- [ ] read typed columns from `TRowSet`;
- [ ] evaluate expressions;
- [ ] update masks/selections;
- [ ] normalize sort/radix keys;
- [ ] update aggregate state;
- [ ] probe/update join state;
- [ ] perform materialization loops where the logic is type-specific.

## Kernel Spec Layer

- [x] Add `TOperatorKernelSpec` as the boundary between planner and kernel
  generation.
- [ ] Include:
  - operator kind;
  - input schemas;
  - output schema;
  - referenced columns;
  - expression ASTs;
  - key descriptors;
  - state descriptors;
  - source modules required by the generated module;
  - entrypoint names and ABI signatures.
- [x] Keep the first version internal to qdb runtime; do not expose it as a
  user-facing API.
- [x] Add debug printing for specs to make generated kernels auditable.
- [x] Build and print specs for current filter/project/aggregate/sort compute
  kernels in diagnostics.
- [ ] Add a mode to dump generated oz source/AST for a query.

## Oz Source Modules

- [ ] Move reusable row access helpers into source modules.
- [ ] Move null/validity helpers into source modules.
- [ ] Move fixed-width and string materialization helpers into source modules.
- [ ] Keep generic radix helpers in `radix.oz`; qdb-specific wrappers stay thin.
- [ ] Move aggregate hash table/update/finalize helpers into source modules.
- [ ] Move join hash table/probe/update helpers into source modules.
- [ ] Keep external declarations only for low-level C++ functions that cannot
  reasonably be oz yet.
- [ ] Avoid qdb-specific prefixes in generic source modules; keep prefixes only
  for qdb ABI entrypoints.

## AST Generation Strategy

- [ ] Start with qdb-side `TExprPtr`/AST generation.
- [x] Add a small `NOz::FunBuilder`/`NOz::ModuleBuilder` instead of manually creating
  AST fragments in many files.
- [ ] Builder should cover common constructs:
  - [x] function declarations;
  - [x] typed params;
  - [x] local variables;
  - [x] loops;
  - [x] if/else;
  - [x] calls;
  - [x] pointer/array indexing;
  - [x] typed literals;
  - [x] returns.
- [ ] Generated oz should look like source a human could write.
- [ ] Keep generated names stable enough for perf/JIT symbols.
- [ ] Defer oz-lang comptime/macros until repeated patterns are clear.

## Generic Runtime Shells

- [x] Introduce `TRuntimeUnaryStreamingKernel`.
- [x] Introduce `TRuntimeUnaryBlockingKernel`.
- [x] Introduce `TRuntimeBinaryKernel`.
- [ ] Keep `TRuntimeSource` and sink code separate.
- [ ] Shell inputs:
  - child runtime nodes;
  - output type;
  - compiled kernel bundle;
  - state factory;
  - batch size/output policy.
- [ ] Shell responsibilities:
  - pull input rowsets;
  - retain/release rowsets;
  - allocate reusable scratch/state;
  - call kernel entrypoints;
  - publish output rowsets;
  - destroy state.
- [ ] Operator-specific C++ classes become thin adapters or disappear.

## Parallel Execution Compatibility

This plan should be implemented with the future fragment/connection executor in
mind (`PLAN_PARALLEL_PHYSICAL_EXECUTION.md`).

- [ ] Runtime shell state must be per-fragment/per-node, not hidden global
  mutable state.
- [ ] Compiled kernel bundles may be shared only if they are immutable and
  thread-safe after construction.
- [ ] Scratch buffers must be owned by the runtime shell instance that uses
  them.
- [ ] Rowset ownership must remain explicit through `Retain/Release`, so a
  future connection can transfer rowsets across worker threads safely.
- [ ] Generated oz kernels should not assume that upstream and downstream run on
  the same OS thread.
- [ ] Kernel specs should expose partition/sort/key descriptors clearly enough
  for a future physical split pass to reason about shuffle keys and local/final
  operators.
- [ ] Blocking unary shells should make state boundaries explicit, because these
  are natural split/merge points for future local+final execution.
- [ ] Binary shells should keep input-side state separated, so hash-partitioned
  join fragments can own independent per-partition state later.
- [ ] Do not introduce APIs that require parent/child direct calls across all
  operators; future parallel execution will replace some edges with
  connections.

## Migration Order

### Stage 0 - Inventory And Baseline

- [ ] List all current runtime nodes and their custom responsibilities.
- [ ] Record C++ LOC for `qdb/exec/*` and `qdb/kernel/*gen*`.
- [ ] Record TPC-H SF1/SF10 timings for current baseline.
- [ ] Record which functions dominate perf for Q9/Q17/Q21.
- [ ] Add a checklist of behavior tests that must stay green.

### Stage 1 - Kernel Spec And Builder

- [x] Add `TOperatorKernelSpec`.
- [x] Add initial oz AST function builder.
- [x] Convert one existing simple kernel generator to the builder.
- [x] Add golden/debug dump tests for kernel specs.
- [x] Start using filter/project kernel specs in diagnostics without changing
  runtime behavior.
- [x] Do not change runtime behavior in this stage.

### Stage 2 - Filter And Materializing Project

- [x] Express filter as a unary streaming kernel spec.
- [x] Express computed project as a unary streaming kernel spec.
- [x] Keep zero-copy project as `non-compute`.
- [x] Share one generic unary streaming C++ shell.
- [x] Remove filter/project-specific C++ dispatch duplication.
- [x] Tests:
  - [x] filter selection semantics;
  - [x] project computed columns;
  - [x] string literals/string views;
  - [x] null propagation where currently supported.

### Stage 3 - Aggregate

- [x] Represent aggregate state through a kernel spec.
- [ ] Generate aggregate dispatch/finalize entrypoints through oz builder.
- [ ] Move reducer update/finalize logic into oz source modules.
- [x] Use generic unary blocking shell for aggregate.
- [ ] Keep C++ only for state ownership, input pull loop, and output publication.
- [ ] Tests:
  - [x] existing aggregate unit tests;
  - [x] nullable reducer args;
  - [x] grouped and ungrouped aggregate;
  - [ ] TPC-H aggregate queries.

### Stage 4 - Sort And Top-Sort

- [x] Represent sort keys as kernel key descriptors.
- [ ] Move numeric key extraction/normalization fully into oz source modules.
- [x] Use generic unary blocking shell for full sort.
- [x] Use same shell plus bounded state policy for top-sort.
- [ ] Keep C++ fallback only for string-key comparison until string kernels are
  ready.
- [x] Remove unsupported degenerate fallbacks except string sort path.
- [ ] Tests:
  - [x] sort/top-sort unit tests;
  - [x] nullable key ordering;
  - [x] multi-key ordering;
  - TPC-H order/limit queries.

### Stage 5 - Join

- [x] Represent join as a binary kernel spec.
- [x] Move hash/probe/update code into oz source modules.
- [x] Keep C++ state shell for rowset retention and output publication.
- [x] Generate join entrypoints from key and residual descriptors.
- [x] Share binary shell across inner/semi/anti/outer join modes where possible.
- [x] Keep residual filters in oz kernels.
- [x] Do not add new join algorithms in this stage; keep the current algorithmic
  behavior and only change the lowering/runtime structure.
- [x] Treat grace/spill/merge/mark/delim join work as separate low-priority
  plans unless a benchmark makes them necessary.
- [x] Tests:
  - [x] join kernel tests;
  - [x] join execution tests;
  - [x] residual semi/anti tests;
  - [x] Q21.

Stage 5 completion notes:

- [x] Extract residual semi/anti finalization from `TRuntimeJoin::Next`.
- [x] Normalize `TRuntimeJoin::Next` into a small mode dispatch after
  finalization extraction.
- [x] Re-evaluate whether the binary shell is shared enough across
  inner/semi/anti/outer modes to mark the shell-sharing item done.
- [x] Re-evaluate whether C++ is now only state ownership, rowset retention, and
  output publication for joins.

### Stage 6 - Delim Join Readiness

- [ ] Make delimiter state another binary/blocking shell policy, not a bespoke
  runtime node if possible.
- [ ] Reuse kernel spec machinery for delimiter key extraction.
- [ ] Reuse aggregate/join source modules for inner correlated aggregate.
- [ ] Keep this blocked until `PLAN_DELIM_JOIN.md` becomes active.
- [ ] This stage is explicitly optional and low priority for the node-lowering
  goal.

Stage 6 status: deferred. Do not start this as part of the current node-lowering
merge unless `PLAN_DELIM_JOIN.md` is explicitly activated.

## JS Dispatch Feasibility Roadmap

This is implementation planning, not part of the architectural dispatch
contract. The architecture is described in `docs/arch/dispatch.md`.

- [ ] Review the dispatch architecture doc and decide whether the node-kind set
  is sufficient: `source`, `non_compute`, `unary_stream`, `unary_block`,
  `binary_join`, `sink`.
- [ ] Define the serialized runtime plan shape for JS dispatch:
  - node id;
  - node kind;
  - child node ids;
  - output schema;
  - shell config;
  - kernel bundle id.
- [ ] Prototype JS dispatch for `source` + `unary_stream`:
  - filter;
  - computed project;
  - zero-copy project as `non_compute`.
- [ ] Prototype JS dispatch for `unary_block` aggregate while keeping state
  allocation and kernels in the host side.
- [ ] Decide whether sort/top-sort need an explicit state-policy descriptor
  before JS dispatch.
- [ ] Prototype JS dispatch for `binary_join` by porting the current
  `TRuntimeJoin` state machine directly, without inventing a new generic binary
  abstraction first.
- [ ] Keep ABI details out of this roadmap until the dispatch model is reviewed.

## C++ Reduction Targets

- [ ] Runtime nodes should contain control flow, not expression/type-specific
  logic.
- [ ] Most type switches should move from C++ runtime into oz source modules or
  kernel generation.
- [ ] `qdb/exec/*` should converge to shells and state containers.
- [ ] `qdb/kernel/gen.cpp` should converge to spec-to-oz builders, not
  hand-coded operator algorithms.
- [ ] Adding a new unary compute operator should require:
  - logical operator;
  - kernel spec builder;
  - oz source helper if needed;
  - no new large C++ runtime node.

## Open Design Questions

- [ ] Should generated kernels be emitted as oz source text, `TExprPtr` AST, or
  both?
- [ ] How much of rowset output materialization should be oz-owned versus
  C++-owned?
- [ ] Do we need a first-class typed `Selection`/row-index vector abstraction?
- [ ] Should masks remain byte-per-row selections or move toward Arrow-style
  bitmaps internally?
- [ ] Where should kernel cache keys live?
- [ ] How stable do generated function names need to be for perf/JIT tooling?
- [ ] Which parts deserve future oz-lang comptime support?

## Oz Comptime Position

- [ ] Do not start by adding oz comptime/macros.
- [ ] First, generate oz AST/source from qdb.
- [ ] Keep generated oz readable and close to what a future comptime API would
  produce.
- [ ] After two or three operators are migrated, identify repeated generation
  patterns.
- [ ] Add oz comptime only for proven patterns that reduce qdb-side generation
  complexity.

## Acceptance Criteria

- [ ] TPC-H SF1 remains green after every stage.
- [x] Existing unit tests remain green after every stage.
- [ ] Generated oz can be dumped and inspected for a failing query.
- [ ] Adding a new simple unary operator does not require a bespoke runtime
  class.
- [ ] C++ runtime code shrinks or stops growing despite adding features.
- [ ] Perf does not regress materially; where it does, the generated oz path is
  inspectable enough to optimize.
