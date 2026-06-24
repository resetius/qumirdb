# Join reorder

`ReorderJoins` rewrites each left-deep chain of cross joins (the comma-join shape
the builder emits) into a **connected** order, so that adjacent joins always have
a real equality between them. Only genuinely disconnected relations stay
cross-joined. It runs **before** [predicate_pushdown.md](predicate_pushdown.md):
reorder fixes the tree *shape*, equijoin then lifts the keys.

## Source map

| Area | Source |
|---|---|
| Pass | `qdb/plan/passes/join_order.{h,cpp}` (`ReorderJoins`) |
| Conjunct/disjunct splitting | `qdb/plan/passes/{flatten_conjucts,flatten_disjuncts,factor_conjuncts}.cpp` |

## Pipeline position

Runs after `QualifyColumns` + `AnnotateTypes` (it needs **qualified** column
names and leaf output schemas to map a column to its relation), and before
`ExtractEquiJoins`. `AnnotateTypes` is re-run afterwards because the chain is
rebuilt.

## Why it is needed

The builder emits the FROM order verbatim. `FROM part, supplier, lineitem` is
left-deep `((part × supplier) × lineitem)`, but `part` and `supplier` share no
predicate — they only connect *through* `lineitem`. Equijoin extracts keys on the
given shape; it cannot invent a join order, so `part × supplier` would stay a
real cross product.

## Algorithm

Two phases over each reorderable chain.

A join is **reorderable** iff it is `inner`, has empty keys, and has no residual
(exactly the comma-join shape; explicit-`ON`, semi/anti, outer joins are opaque).

### 1. Collect edges and leaves

`Reorder(node, edges)` threads equality edges down from enclosing filters:

- `filter`     → add the predicate's `col == col` equalities to `edges`, recurse.
- `join` (any) → pass `edges` to both children.
- `project`/`aggregate` → recurse with **empty** edges (columns are redefined).
- reorderable chain → flatten to its leaf operators, reorder each leaf with empty
  edges, then rebuild (phase 2).

Edges are collected with `FactorConjuncts` (so an equality factored out of a
disjunction also counts) and keep both operand names. Because names are
qualified, an edge from one scope never matches leaves of another.

### 2. Greedy connected order (`BuildChain`)

Map each output column to its owning leaf (via `OutputColumns`). Each edge whose
both endpoints are owned by distinct leaves adds an adjacency. Then:

```
order = []
while some leaf unplaced:
    seed = first unplaced leaf            # starts a new component (a cross)
    place seed
    repeat: place any unplaced leaf adjacent to an already-placed one
rebuild left-deep: ((order[0] × order[1]) × order[2]) ...   # inner, empty keys
```

Each appended leaf shares an edge with the already-placed side, so the join that
attaches it has a straddling equality → equijoin lifts a key there. Components
that share no edge are concatenated (an unavoidable cross).

## Examples

### Comma join (q9)

```
FROM part, supplier, lineitem, partsupp, orders, nation
WHERE p_partkey=l_partkey AND s_suppkey=l_suppkey AND ps_suppkey=l_suppkey
  AND ps_partkey=l_partkey AND o_orderkey=l_orderkey AND s_nationkey=n_nationkey
```

Edges: part–lineitem, supplier–lineitem, partsupp–lineitem, orders–lineitem,
supplier–nation. Greedy from `part`:

```
part, lineitem, supplier, partsupp, orders, nation
=> ((((part × lineitem) × supplier) × partsupp) × orders) × nation
```

Every join now has a direct equality; equijoin lifts a key on each. `supplier`
attaches via `s_suppkey=l_suppkey`, `nation` via `s_nationkey=n_nationkey`, etc.

### Chain under a decorrelation LEFT JOIN (q2)

Scalar decorrelation wraps the outer query in a `LEFT JOIN`, leaving the WHERE
filter *above* it and the inner chain *under* its left input:

```
filter (... AND ps_supplycost == __scalar_0__)         -- holds the chain's edges
└─ join left [p_partkey = __corr_0_0__]
   ├─ <inner chain: part, supplier, partsupp, nation, region>   (cross)
   └─ <decorrelated subquery>
```

The filter's edges (`p_partkey=ps_partkey`, `s_suppkey=ps_suppkey`,
`s_nationkey=n_nationkey`, `n_regionkey=r_regionkey`) are threaded **through** the
LEFT join into the inner chain. `ps_supplycost == __scalar_0__` is also an edge
but `__scalar_0__` is not a chain leaf, so it is ignored when ordering the chain.
Result:

```
((((part × partsupp) × supplier) × nation) × region)
```

The subquery on the right side is reordered independently using its own filter's
edges (edges reset across the project/aggregate above it).

## Non-goals

No cost model: ordering is purely connectivity-driven, seeded from the first
relation. Cardinality-aware ordering is future work.
