# Architecture

Logical planning (SQL → optimized logical plan), in pipeline order:

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
- [String values](string_values.md) - proposed borrowed/owned string ABI for
  filtering and aggregation, including dual lookup/storage key types.
