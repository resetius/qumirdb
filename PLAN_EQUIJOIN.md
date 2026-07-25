# Equi-join extraction (cross + filter → hash joins)

Optimizer pass that turns the builder's naive output — a left-deep tree of
empty-key inner joins (cross products) plus predicates in residuals/filters —
into joins with real `TJoinKey`s, reordering the cross region so every join has
a connecting equality. Home: `qdb/plan/passes/equijoin.{h,cpp}`.

## Preconditions

The pass is schema-dependent, so it runs **after** the semantic passes:

```
builder → AssignSourceAliases → QualifyColumns → AnnotateTypes
        → [equijoin extraction]            ← this pass
        → predicate pushdown → ApplyColumnPruning
```

It relies on:
- `QualifyColumns` — every column reference is a unique `alias.col`.
- `AnnotateTypes` — every operator has input/output `TStructType` schemas.
- `FindUnboundVars` (`plan/passes/unbound_vars.h`) — column names used by an expr.

## What it rewrites

Two shapes carry extractable equalities:
1. **comma-FROM region** — `filter(pred)` over a chain of inner empty-key joins.
   The whole region is reorderable.
2. **explicit `JOIN ON`** — a single inner join with empty keys and the `ON`
   predicate as residual. Two fixed inputs, no reorder, just split the residual.

Fixed points (do **not** reorder across them; extract keys in place from their
residual only):
- outer joins (`Left`/`Right`/`Full`) — reordering changes semantics;
- semi/anti joins (from decorrelation).

## Equality is transitive — use equivalence classes, not literal edges

`t1.a = t2.b AND t2.b = t3.c` also implies `t1.a = t3.c`. The literal predicates
give edges `(t1,t2)` and `(t2,t3)` only, which would force the order
`t1 ⋈ t2 ⋈ t3` and forbid joining `t1` with `t3` directly. A real optimizer
must take the **transitive closure** of equalities so any pair of relations
sharing a column class can be joined.

So instead of literal edges, group equi columns into **equivalence classes**
with union-find (`qdb/utils/union_find.h`, `TUnionFind<std::string>`):

- for every conjunct of the shape `a = b` where `a`, `b` are column references in
  **different** relations → `uf.Union(a, b)`.
- each resulting class is a set of mutually-equal columns; any two columns in the
  same class, owned by two different relations, are a valid join key.

A join between relations `R` and `S` exists iff some column of `R` and some
column of `S` share a class. The key is `(R-column, S-column)` — any
representatives. Only a **spanning set** of keys per class is needed (enough to
connect the relations); the rest of the pairwise equalities are implied and
dropped.

## Core operation: classify a predicate set against a set of leaves

Given leaves `L0..Ln` (each an operator with an output `TStructType`) and a list
of conjuncts:

- `Relations(expr)` = `{ leaf : leaf.schema ∩ FindUnboundVars(expr) ≠ ∅ }`.
- Classify each conjunct by `|Relations|`:
  - **0** (constant) → top filter.
  - **1** → single-relation predicate (leave for pushdown).
  - **2 and shape `a = b`** across two relations → feed into the union-find
    (it becomes part of a class, not a standalone edge).
  - otherwise (≥2 relations, non-equi) → **residual** tied to its relation set.

Column → leaf lookup is schema membership (not alias-string parsing), so it is
robust to naming.

## Two passes, in order

Key extraction and join ordering are **separate**. Do extraction first: it keeps
the builder's tree shape (the FROM order) and only assigns keys. For most of
TPC-H the FROM order is already a connected left-deep chain (e.g. q3:
`(customer ⋈ orders) ⋈ lineitem` — both joins find a key in place), so reordering
is the rarer, second concern.

## Pass 1 — extract keys in place (no reorder)

### Stage A — collect the region
- [ ] find a rewrite site: `TFilterOperator` whose input is an inner empty-key
      `TJoinOperator` (or such a join at the root).
- [ ] flatten the maximal inner empty-key join subtree into `leaves` (operators
      that are not themselves inner empty-key joins: sources, subplans,
      outer/semi/anti joins, already-keyed joins).
- [ ] collect `conjuncts` = filter conjuncts ∪ every region join's residual
      conjuncts (flattened on `&&`).

### Stage B — classify and build equivalence classes
- [ ] build `leafOf: column → leaf` from leaf output schemas.
- [ ] `TUnionFind<std::string> classes;` — `Union(a, b)` for every cross-relation
      equality conjunct (these conjuncts are now consumed).
- [ ] keep `residuals` (multi-rel non-equi), `single` (one-rel), `consts`.

### Stage C — recurse top-down, assigning keys and pushing the rest down
The existing tree shape is kept. Recurse from the top join with the leftover
(non-equality) conjuncts; this unifies key extraction with predicate pushdown:

```
process(node, preds):                 # preds.relations ⊆ node.relations
  if node is inner join (L, R):
     keys = { (Lrep, Rrep) | class has a rep in L and a rep in R }   # transitive
     toL      = { p ∈ preds | Relations(p) ⊆ L }
     toR      = { p ∈ preds | Relations(p) ⊆ R }
     residual = preds \ (toL ∪ toR)                                  # crosses L|R, non-equi
     return join(process(L, toL), process(R, toR), keys, residual)
  else (leaf):
     return preds.empty() ? node : filter(node, conjoin(preds))
```

- [ ] equality conjuncts are already consumed into the classes, so `preds` here
      are only non-equi / single-relation / const predicates.
- [ ] keys come from classes spanning the two sides — a transitive class emits a
      key at every join level where it straddles, so `t1 ⋈ t3` on the implied
      `t1.a = t3.c` is found even though no one wrote it.
- [ ] each leftover predicate sinks to the lowest join that still covers all its
      relations; single-relation ones land as a leaf `filter` (pushdown for free).
- [ ] a join that finds no key stays a cross (keys empty) — Pass 2 fixes that.

### Fixed points
Outer and semi/anti joins are recursion **boundaries**: do not push `preds`
across them and do not feed their residual columns into the shared classes
(inner columns of a semi/anti join are not visible above). Extract their keys
**in place** from their own residual (`a = b` across the two sides → key), then
recurse into each side with only that side's predicates.

## Pass 2 — join ordering (later)

Only needed when Pass 1 leaves a keyless inner join (a cross) in the middle —
i.e. the FROM order is not connected. Rebuild that inner region's order from the
class graph: greedily add the leaf that shares a class with the joined set
(`availableCols`), assigning keys as above; a disconnected component forces an
explicit cross-bridge. Start with greedy connectivity (no cost model); add
cardinality/selectivity-based ordering later. Outer/semi/anti joins are fixed
boundaries that ordering does not cross.

## Data structures (sketch)

```cpp
struct TLeaf { TOperatorPtr Op; const TStructType* Schema; };

TUnionFind<std::string> classes;                  // column equivalence classes
std::unordered_map<std::string, size_t> leafOf;   // qualified column -> leaf index
```

Keys between two sides `L` and `R` are found by, for each class, checking whether
it has a representative column in `L`'s schema and one in `R`'s; if so emit
`TJoinKey{ Lrep, Rrep }`. Grouping columns by `classes.Find(col)` once up front
makes this a lookup rather than a scan. Both passes share this routine.

## Scope and phasing

- [ ] **Pass 1 — key extraction in place** (this is the first deliverable): no
      reorder, equivalence classes for transitive keys, in-place keys for
      `JOIN ON` and outer/semi/anti. Covers the TPC-H queries whose FROM order is
      already connected.
- [ ] **Pass 2 — greedy join ordering** (connectivity, no cost model): only the
      inner cross region, only when Pass 1 leaves a middle cross.
- [ ] **Pass 2+ — cost-based ordering** (cardinality/selectivity) — later.
- [ ] residual placement refinement (push a residual to the lowest covering
      join) — partly here, fully with pushdown.

## Open questions

- **Name ambiguity after decorrelation**: a decorrelated subquery may reuse a
  column name present in the outer query (e.g. `l_orderkey` in q18). Either the
  decorrelation must alias the right side, or this pass must disambiguate via
  per-leaf schema membership rather than bare names. Membership-based `leafOf`
  handles it as long as `QualifyColumns` gave distinct `alias.col` names.
- **Constant propagation**: a class that also contains a constant equality
  (`t1.a = 5` with `t1.a` in the class) lets `5` be pushed onto every relation in
  the class as a single-relation filter (`t2.b = 5`, …) — strong pruning. Fold
  constants into the same union-find structure; a later extension.

## Testing

- [ ] post-pass goldens (`*.opt.plan`): run builder + qualify + type + this pass,
      diff against goldens; compare shapes with hand-written
      `benchmark/tpch/queries/*.sexp`.
- [ ] start with q3, q5, q10 (clean comma-FROM equi chains).
