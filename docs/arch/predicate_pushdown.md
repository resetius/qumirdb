# Predicate pushdown & equi-key extraction

`ExtractEquiJoins` is one pass that does three things over the whole tree:

1. lifts equalities from `WHERE`/`ON` into **join keys** (transitively, via
   equivalence classes);
2. **pushes** the remaining predicates down to the side(s) they constrain;
3. leaves only genuine cross-side predicates as a join **residual**.

It runs after [join_reorder.md](join_reorder.md), so the chain shape is already
connected.

## Source map

| Area | Source |
|---|---|
| Pass | `qdb/plan/passes/equijoin.{h,cpp}` (`ExtractEquiJoins`) |
| Equivalence classes | `qdb/utils/union_find.h` |
| Conjunct/disjunct splitting | `qdb/plan/passes/{flatten_conjucts,flatten_disjuncts,factor_conjuncts}.cpp` |
| Free-variable detection | `qdb/plan/passes/unbound_vars.{h,cpp}` |

## Model

The pass walks top-down carrying a **pool** of conjuncts (`TConjuct`). A conjunct
is either an equality between two column idents (`equiv`, with `left`/`right`
names) or an opaque expression.

- `filter`            → drop the node, add its conjuncts to the pool, recurse.
- `project`/`aggregate` → recurse the input with an **empty** pool (a barrier:
  columns are redefined), then materialize the pool's conjuncts above as a filter.
- redistributable `join` (inner, semi, anti) → `ProcessJoin`.
- outer `join` (left, right, full) → `ProcessOuterJoin`.
- leaf (`source`) → materialize the pool above it as a filter.

Conjuncts are obtained with `FactorConjuncts`, which both flattens `&&` and
**factors atoms common to every branch of an `||`** (see below).

`ColumnsOf(conjunct)` = the columns it references. For an opaque expression this
is `FindUnboundVars`, which excludes a call's callee (a function name is not a
column) so that e.g. `like(o_comment, '…')` counts as constraining only
`o_comment` and can be pushed to one side.

## ProcessJoin (inner / semi / anti)

```
pool += join.residual            # fold ON / decorrelation correlation into pool
classes = union-find over equiv conjuncts in pool
left, right = column sets of each input
```

Per equivalence class, split its members by side and emit **one** cross-side key;
reconnect the remaining same-side members with a spanning tree pushed down so a
deeper join can lift its own key:

```
class {a.x, b.x, c.x}  with a,b on left, c on right
=> join key (a.x, c.x)
   push a.x == b.x to the LEFT input        # deeper join re-extracts it
```

This guarantees one key per class (no redundant duplicates) and propagates
transitive equalities to the level that can use them.

Non-equi conjuncts are partitioned by coverage:

```
covers(cols, leftCols)  → push to left
covers(cols, rightCols) → push to right
otherwise               → residual on the join (+ DeriveSideFilter, below)
```

### Worked example

```
input pool (at the top join of a connected 4-way chain):
  l_orderkey == o_orderkey, l_partkey == ps_partkey, s_suppkey == ps_suppkey,
  o_orderdate < D, like(p_name,'%x%')
```

`o_orderkey` straddles → key; `o_orderdate < D` covers orders → pushed onto
orders; `like(p_name,…)` covers part → pushed onto part; the partsupp equalities
are pushed/keyed at the deeper joins.

## Disjunctions: factoring and DeriveSideFilter

`FactorConjuncts` rewrites `(a∧x) ∨ (a∧y) ≡ a ∧ (x∨y)` so a join equality buried
in an `OR` is exposed (TPC-H q19): the common `p_partkey == l_partkey` is hoisted
to the pool and becomes a key; the reduced `OR` stays as the residual.

For a residual disjunction that still spans both sides, `DeriveSideFilter`
derives a **necessary** single-side condition and pushes it down: for each
disjunct keep only the atoms over one side; if every disjunct constrains that
side, their `OR` is a valid filter for it.

```
(brand=35 ∧ qty∈[10,20]) ∨ (brand=34 ∧ qty∈[18,28])
  left  (part):     brand=35 ∨ brand=34         → pushed to part
  right (lineitem): qty∈[10,20] ∨ qty∈[18,28]   → pushed to lineitem
  full OR stays as the join residual
```

## ProcessOuterJoin (left / right / full)

The executor has **no residual support for outer joins**, so the pass must empty
`join.residual`.

- Equi keys are lifted from the `ON` only (WHERE conjuncts from above are *not*
  folded into the ON of an outer join).
- A non-key `ON` conjunct that constrains **only the null-extended side** is
  pushed onto that side *before* the join — equivalent for an outer join:

```
customer LEFT JOIN orders ON c_custkey=o_custkey AND not like(o_comment,'%x%')
=>
join left [c_custkey = o_custkey]
├─ source customer
└─ filter (not like o_comment '%x%')        # pushed to the null side
   └─ source orders
```

  Putting that predicate *above* the join would be wrong: a customer whose only
  orders match the comment would get matched rows, then dropped, instead of
  appearing null-extended with `count = 0`.
- `WHERE` conjuncts from above are pushed only onto the **preserved** side; the
  rest are materialized above the join.

## Invariants after the pass

- inner/semi/anti joins may carry keys and a residual;
- outer joins carry keys and **no** residual;
- a `filter` remains only where a predicate could not be pushed further down.
