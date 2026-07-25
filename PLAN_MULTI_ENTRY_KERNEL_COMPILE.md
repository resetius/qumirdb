# Multi-Entry Kernel Compile Plan

## Goal

Compile one generated kernel module once per physical stage, then lookup all
needed entry points from the same live JIT module.

This removes repeated AST transform, Qumir IR lowering and LLVM codegen for
modules where many functions share support code.

## Current Waste

| Kernel family | Current entry points | Current compile count |
|---|---:|---:|
| Join | `jt_init`, `jt_process_left`, `jt_process_right`, `jt_probe_left_stream`, `jt_probe_right_stream`, `jt_destroy`, `pb_destroy` | 7 per join stage |
| Join hash shuffle | `jt_hash_left`, `jt_hash_right` | 2 per join stage |
| Aggregate | update / measure / finalize | 3 per aggregate stage |

These are per-stage compile counts, not per-task compile counts. Runtime code is
already shared by tasks; the duplicated work is inside stage compilation.

## Implementation

1. [x] Add a multi-entry compile API to Qumir LLVM runner.

   It should compile/lower/codegen one AST module once and return function
   pointers for several entry names. All pointers must keep the same live JIT
   owner alive. Keep the existing single-entry API as a wrapper or alongside it.

2. [x] Convert qdb kernels that already build one shared module.

   Start with `CompileJoinHash()` and `CompileJoin()`, because they currently
   rebuild the same module for every entry. Then apply the same pattern to
   aggregate update/measure/finalize if their generated support code is shared
   enough to benefit.

3. [x] Keep public kernel ABIs unchanged.

   `TJoinKernels`, `TJoinHashKernels` and `TAggregateKernels` should expose the
   same callables as now. Only their captured JIT/module owner changes from
   per-entry owner to shared per-stage owner.

4. [ ] Evaluate aggregate separately.

   Aggregate currently builds update, measure and finalize as separate AST
   programs. Merging them is possible, but it is not the same direct replacement
   as join/join-hash and should be done as a separate small change.

## Verification

- Existing tests must pass.
- TPC-H smoke must pass for the affected queries:
  - Q9 and Q21 for join/join-hash;
  - Q1 for aggregate.
- Explain UI should no longer show repeated full-module AST artifacts per join
  entry; one stage should correspond to one compiled module with multiple
  entries.

## Non-Goals

- No scheduler execution changes.
- No kernel ABI changes.
- No code cloning per task.
- No new tests for this refactor unless an existing failure needs a focused
  reproducer.
