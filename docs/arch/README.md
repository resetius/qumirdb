# Architecture

## Query pipeline

SQL → logical plan → idempotent rewrite passes → physical plan. Entry point
`bin/cli.cpp` (`RunQuery`); builder `qdb/plan/build.cpp`; passes
`qdb/plan/passes/`.

1. **Build** — SQL AST → naive operator tree (no keys, no pushdown, no reorder).
   [logical_plan_build.md](logical_plan_build.md), [decorrelation.md](decorrelation.md).
2. **AssignSourceAliases** — unique alias per source.
3. **QualifyColumns** — rewrite refs to `alias.col`; set source/join schemas.
4. **AnnotateTypes** — attach `TFunctionType` (input/output schema) to every op;
   idempotent, re-run after each structural rewrite.
5. **ReorderJoins** — cross-join chains → connected order.
   [join_reorder.md](join_reorder.md).
6. **ExtractEquiJoins** — lift equi-keys (equivalence classes), push predicates
   per side, leave residuals. [predicate_pushdown.md](predicate_pushdown.md).
7. **ApplyColumnPruning** — narrow each op's required input columns.
8. **AttachRowGroupPredicates** — attach a conservative Parquet row-group
   pruning hint without removing the row-level filter.
   [predicate_pushdown.md](predicate_pushdown.md#parquet-row-group-pruning).

Detailed algorithm docs:

- [Logical plan build](logical_plan_build.md) - SQL AST → naive operator tree:
  FROM/WHERE/SELECT/GROUP BY/HAVING mapping, aggregate-argument materialization,
  global-aggregate and `count(distinct)` rewrites, CTE inlining, derived-table
  aliases.
- [Join reorder](join_reorder.md) - rewriting cross-join chains into a connected
  order by threading filter equalities down to each chain.
- [Predicate pushdown](predicate_pushdown.md) - equi-key extraction via
  equivalence classes, per-side predicate pushdown, disjunction factoring and
  derived side filters, outer-join residual handling.
- [Decorrelation](decorrelation.md) - EXISTS/IN → semi/anti joins; scalar
  subqueries → broadcast cross join (uncorrelated) or group-by + LEFT join
  (correlated, Apply elimination).

Physical execution:

- [Aggregation](aggregation.md) - aggregation planning, generated Qumir AST,
  generic Oz kernels, type-specific hash injection, runtime ABI and memory.
- [Join](join.md) - supported join forms, adaptive symmetric/probe-only hash
  join scheduling, kernel split, residual predicates, and runtime lifetime.
- [Scheduler runtime](scheduler_runtime.md) - scheduled physical graph, task
  nodes, connections, compiled kernels, lowering patterns, and JS/WASM runtime
  boundary.
- [Dispatch](dispatch.md) - runtime shell dispatch model, node kinds, and
  kernel/dispatcher ownership split.
- [String values](string_values.md) - proposed borrowed/owned string ABI for
  filtering and aggregation, including dual lookup/storage key types.
- [SQL expression pseudo-calls](sql_pseudo_calls.md) - call-shaped parser and
  typing markers for `IN`, concatenation, and SQL NULL semantics.
