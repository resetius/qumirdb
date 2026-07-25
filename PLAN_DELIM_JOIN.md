# Delim Join Plan

Goal: plan support for correlated subquery decorrelation through a `delim`
mechanism, primarily for TPC-H Q17-style queries where the inner aggregate
should be restricted by distinct keys produced by the outer side.

This is a planning document only. Do not implement until we decide it is worth
the runtime and planner complexity.

## Motivation

DuckDB's Q17 plan does not evaluate the scalar correlated aggregate as an
independent second scan of filtered `part`. It builds the outer relation
`lineitem JOIN filtered part`, extracts the relevant `p_partkey` values through
a delimiter side channel, computes:

```text
lineitem JOIN delim_keys(p_partkey)
-> aggregate by p_partkey: avg(l_quantity)
```

and joins the per-key threshold back to the outer rows.

Our current hand-written Q17 approximation is:

```text
outer: lineitem JOIN filtered part
threshold:
  lineitem LEFT_SEMI filtered part
  -> aggregate by l_partkey: sum/count
outer JOIN threshold
filter l_quantity < threshold
```

This is semantically fine for the benchmark shape, but it cannot express the
general "inner subplan depends on keys emitted by the outer subplan" execution
strategy.

## Target Query Shape

- [ ] Correlated scalar aggregate subquery:
  `x < (select agg(y) from inner where inner.k = outer.k)`.
- [ ] Correlation predicates are equality predicates over one or more keys.
- [ ] Inner subquery returns at most one row per correlated key after aggregate.
- [ ] Outer query may reference the subquery result in a filter or projection.
- [ ] Initial target is Q17:
  `l_quantity < 0.2 * avg(l_quantity) where inner.l_partkey = outer.p_partkey`.

## Logical Operators

- [ ] Add a logical `TDelimJoinOperator`.
- [ ] Add a logical `TDelimScanOperator` or `TDelimSourceOperator`.
- [ ] Represent a delimiter id shared by `TDelimJoinOperator` and all matching
  `TDelimScanOperator` nodes.
- [ ] Decide whether delimiter payload is:
  - distinct key rows only;
  - full outer rows;
  - or key rows plus an optional row id back-reference.
- [ ] For Q17 MVP, delimiter payload can be distinct key rows only.
- [ ] `TDelimJoinOperator` output schema should normally be the joined schema:
  outer columns plus subquery result columns.
- [ ] `TDelimScanOperator` output schema should be the delimiter key schema.

## Planner Rewrite

- [ ] Detect correlated scalar aggregate subqueries after SQL parse and before
  regular join-order optimization.
- [ ] Extract equality correlations into delimiter keys.
- [ ] Build outer input once.
- [ ] Build inner subplan with correlated references replaced by a
  `TDelimScanOperator`.
- [ ] Aggregate inner subplan by delimiter keys.
- [ ] Join aggregate result back to the outer input.
- [ ] Preserve scalar-subquery NULL semantics:
  no inner match means subquery result is NULL.
- [ ] Preserve scalar cardinality semantics for non-aggregate scalar subqueries
  in a later phase; Q17 aggregate shape does not need this first.

## Runtime Model

- [ ] `TRuntimeDelimJoin` is a pipeline breaker on the outer side.
- [ ] Phase 1: consume outer input, retain batches, and collect distinct
  delimiter keys.
- [ ] Phase 2: expose delimiter keys to inner runtime through
  `TRuntimeDelimScan`.
- [ ] Phase 3: execute inner runtime and build a lookup table keyed by delimiter
  key.
- [ ] Phase 4: replay retained outer rows, attach matched inner result columns,
  and emit output batches.
- [ ] For Q17, Phase 4 can emit `outer + threshold` and let an ordinary filter
  apply `l_quantity < threshold`.

## State And Ownership

- [ ] Add shared delimiter state owned by the delim join runtime.
- [ ] State contains:
  - retained outer `TRowSet` batches;
  - distinct key table;
  - a materialized key rowset for `TRuntimeDelimScan`;
  - inner result lookup table.
- [ ] `TRuntimeDelimScan` must not outlive its owning delim state.
- [ ] Define clear lifetime for retained outer batches with `Retain/Release`.
- [ ] Reuse aggregation/join key representation where possible.

## Key Representation

- [ ] Start with fixed-width numeric keys.
- [ ] Reuse existing hash key descriptor work from aggregate/join.
- [ ] Add composite delimiter keys after the single-key path is stable.
- [ ] Add string keys only after string join keys are mature.
- [ ] NULL correlation semantics:
  SQL equality correlation should not match NULL keys unless rewritten with
  `IS NOT DISTINCT FROM`.

## Q17 Target Plan

Desired logical shape:

```text
project avg_yearly = sum_price / 7.0
└─ aggregate sum_price = sum(l_extendedprice)
   └─ filter l_quantity < threshold
      └─ delim-join keys=[p_partkey]
         ├─ outer:
         │  lineitem JOIN filtered(part brand/container)
         └─ inner:
            project threshold = 0.2 * avg_qty
            └─ aggregate keys=[p_partkey] avg_qty=avg(l_quantity)
               └─ lineitem JOIN delim-scan[p_partkey]
```

This avoids the second filtered `part` scan in the threshold branch and matches
the main idea of DuckDB's `LEFT_DELIM_JOIN`/`DELIM_SCAN` plan.

## Execution Semantics

- [ ] For scalar aggregate subquery, missing inner result produces NULL.
- [ ] If the outer filter compares with NULL, normal filter semantics drop the
  row.
- [ ] Duplicate outer keys must produce one delimiter key but all matching outer
  rows must be replayed.
- [ ] Inner aggregate groups by delimiter key, not by outer row id.
- [ ] The operator is blocking with respect to outer input and inner input.

## Tests

- [ ] Unit test: delimiter scan sees distinct keys from outer input.
- [ ] Unit test: duplicate outer keys replay all outer rows.
- [ ] Unit test: missing inner aggregate result yields NULL.
- [ ] Unit test: Q17-sized miniature with two parts and several lineitem rows.
- [ ] SQL regression: correlated aggregate is rewritten to delim join.
- [ ] Sexpr parser/printer support only if we want hand-written delim plans.
- [ ] TPC-H Q17 result check against current plan and DuckDB output.

## Risks

- [ ] This introduces a new cross-subtree state edge; the planner must keep
  ownership explicit so runtime construction is deterministic.
- [ ] It is a pipeline breaker and may retain a large outer side.
- [ ] General scalar subquery semantics are more complex than Q17 aggregate
  semantics.
- [ ] Join-order optimization must not reorder `TDelimScan` away from its
  owning `TDelimJoin`.
- [ ] Column pruning must preserve delimiter keys even if they are not visible
  in final output.

## Non-Goals For First Version

- [ ] Non-equality correlations.
- [ ] Multi-row scalar subquery cardinality errors.
- [ ] `IN`/`EXISTS` mark joins.
- [ ] Nested delim joins.
- [ ] Spill/grace partitioning.
- [ ] Parallel delimiter production.

