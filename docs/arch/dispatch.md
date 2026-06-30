# Dispatch architecture

This document describes the execution-dispatch model after the node-lowering
work. It is intentionally about dispatch and ownership, not about ABI details.
The motivating question is whether a non-C++ dispatcher, such as a JavaScript
dispatcher, can drive execution while C++ still builds and compiles kernels.

## Node kinds at a glance

| Node kind | Examples | Dispatcher job | Kernel job |
|---|---|---|---|
| `source` | table/parquet/text source | Produce rowsets | None |
| `non_compute` | zero-copy project, limit-like views | Rewrite/view rowsets | None |
| `unary_stream` | filter, computed project | Pull one batch, call kernel | Predicate/expression eval |
| `unary_block` | aggregate, sort, top-sort | Drain input, own state | Update/finalize/key work |
| `binary_join` | inner/semi/anti/outer equi-join | Schedule two inputs, drain output | Key/hash/probe/residual |
| `sink` | CLI/result writer/future materializer | Consume rowsets | Usually none |

The dispatch question is mostly about implementing the `Dispatcher job` column
outside C++. The `Kernel job` column should stay in compiled kernels.

## Current split

The current physical executor has three responsibilities:

- plan lowering: choose a runtime shell and build `TOperatorKernelSpec`;
- kernel compilation: generate and JIT the query-specific Oz entrypoints;
- dispatch: pull rowsets through runtime nodes and call compiled kernels.

After lowering, most compute-specific behavior is no longer embedded directly
in per-operator runtime nodes. The runtime shell owns rowset lifetimes, state
objects, scratch buffers, and output publication. Kernels own expression
evaluation, key extraction, hash-table update/probe, and residual predicates.

```text
logical plan
  -> physical planner
      -> TOperatorKernelSpec
      -> compiled kernel bundle
      -> runtime shell
          -> pull inputs
          -> call kernel entrypoints
          -> publish TRowSet output
```

## Operator ownership

Source:

- dispatcher-owned: file/source scan, column restriction, rowset production;
- kernel-owned: none.

Filter:

- dispatcher-owned: input pull, selection buffer lifetime;
- kernel-owned: predicate evaluation, selection update.

Computed project:

- dispatcher-owned: input pull, output buffers, borrowed/owned column assembly;
- kernel-owned: expression evaluation.

Zero-copy project:

- dispatcher-owned: column view construction;
- kernel-owned: none.

Aggregate:

- dispatcher-owned: input drain, hash-table buffer lifetime, output publication;
- kernel-owned: key extraction, state update, finalize materialization helpers.

Sort/top-sort:

- dispatcher-owned: input drain, sort state, bounded top state;
- kernel-owned: numeric key extraction/normalization where supported.

Join:

- dispatcher-owned: two-input scheduling, row stores, hash-table buffers,
  pair/output draining;
- kernel-owned: key extraction, hash/probe/update, residual filter,
  semi/anti/outer finalize kernels.

Limit:

- dispatcher-owned: offset/limit counters;
- kernel-owned: none.

The important boundary is that the dispatcher does not need to understand the
meaning of a filter predicate, project expression, aggregate reducer, join key,
or join residual. It only chooses when to call already-compiled entrypoints.

## Runtime shell model

The runtime shell kinds are intentionally small:

```text
TRuntimeSource

TRuntimeUnaryStreamingKernel
  child.Next(rowset)
  kernel/process(rowset)
  return rowset

TRuntimeUnaryBlockingKernel
  process(child, out)
  return out

TRuntimeBinaryKernel
  owns left child
  owns right child
  exposes output type
```

`TRuntimeJoin` is currently the concrete binary shell for equi-join modes. It
shares the same state and output machinery across inner, semi, anti, and outer
joins, with mode-specific dispatch methods:

```text
TRuntimeJoin::Next
  EnsureInit
  DrainReadyOutput
  if residual semi/anti -> NextResidualSemiAnti
  if semi/anti          -> NextSemiAnti
  if outer              -> NextOuter
  otherwise             -> NextInner
```

This is still one binary runtime shell. The mode methods are control-flow
policies over the same retained row stores, hash tables, pair buffer, compiled
kernel bundle, and output builder.

## Dispatch graphs

Unary streaming operators are already close to a JS-friendly model:

```text
dispatcher
  pull child batch
  call kernel(batch, shell_state)
  return batch/view
```

Unary blocking operators have one materialization boundary:

```text
dispatcher
  while child.Next(batch):
      retain/read batch as needed
      call update kernel/state process
      release batch when consumed
  call finalize/materialize
  return output batches
```

Join has the richest dispatch policy:

```text
dispatcher
  initialize left/right hash table state

  inner/outer symmetric phase:
      choose side
      pull batch
      retain batch in that side row store
      call ProcessLeft/ProcessRight
      drain pair buffer into output builder

  probe-only phase:
      pull stream batch
      call ProbeLeftStream/ProbeRightStream
      materialize output while stream batch is alive
      release stream batch

  semi/anti finalization:
      drain/build retained side state
      call FinalizeAntiSemi
      publish left-side output

  outer finalization:
      call FinalizeOuter
      publish null-padded unmatched rows
```

Nothing in that loop requires C++ language dispatch specifically. It requires a
dispatcher capable of owning state handles, preserving rowset lifetimes, and
calling the correct compiled kernel entrypoint at each transition.

## JS dispatcher target

A JS dispatcher can be modeled as an interpreter over a physical runtime graph:

```text
RuntimePlan
  nodes:
    id
    kind: source | unary_stream | unary_block | binary_join | limit | ...
    children: [ids]
    output_schema
    shell_config
    kernel_bundle_id

KernelBundleRegistry
  id -> compiled entrypoint table

Dispatcher
  next(node_id) -> RowSetHandle | end
```

For example:

```text
next(filter_node):
  input = next(child)
  if input == end: return end
  call filter_kernel(input)
  return input

next(join_node):
  state = get_join_state(join_node)
  return join_next(state)
```

The dispatcher needs a faithful implementation of the shell state machines, not
knowledge of SQL expressions or generated Oz ASTs.

## What is already enough

The current lowering work already provides the major prerequisites:

- compute operators have typed kernel specs;
- filter/project/aggregate/join build kernels from specs;
- generic unary streaming and unary blocking shells exist;
- binary ownership is isolated in `TRuntimeBinaryKernel`;
- join mode dispatch is now explicit and localized in small methods;
- join residual predicates run inside kernels, including semi/anti cases;
- hash/probe/update logic is in Oz source modules rather than JS/C++ dispatch.

This makes a JS dispatcher realistic if C++ continues to provide:

- physical planning or a serialized `RuntimePlan`;
- kernel compilation and a kernel bundle registry;
- rowset/state allocation primitives.

## What still stays in the dispatcher

A JS dispatcher would still need to implement real runtime policy:

- pull order and EOF transitions;
- blocking-vs-streaming boundaries;
- rowset retain/release discipline;
- output batch draining;
- join symmetric/probe-only transitions;
- semi/anti and outer finalization timing;
- sort/top-sort state policy.

This is expected. Moving these policies into kernels would make kernels own
query graph execution, which is not the goal.

## Remaining uncertainty

The largest remaining modeling questions are not ABI questions:

- whether join policy should remain one `binary_join` shell or split into
  `binary_join_inner`, `binary_join_semi_anti`, and `binary_join_outer`;
- whether sort/top-sort should expose a more explicit state-policy descriptor;
- whether source and sink nodes should use the same `next(node)` protocol in
  JS, or be host callbacks;
- whether runtime plans should reference logical operator names or only shell
  kinds plus kernel bundle ids;
- whether kernel specs are sufficient as the serialized plan format, or whether
  they should remain compiler input only and produce a smaller runtime config.
