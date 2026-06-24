# Logical plan build

SQL AST → an unqualified, untyped logical operator tree. The builder is
deliberately naive: it never extracts join keys, pushes predicates, or reorders
joins — those are optimizer passes that run afterwards (see
[join_reorder.md](join_reorder.md), [predicate_pushdown.md](predicate_pushdown.md),
[decorrelation.md](decorrelation.md)).

## Source map

| Area | Source |
|---|---|
| Builder | `qdb/plan/build.{h,cpp}` (`BuildPlan` → `BuildQuery` → `BuildSelect`) |
| Operators | `qdb/plan/ops/{source,filter,project,aggregate,join}.{h,cpp}` |
| Expression clone (CTE inlining) | `qdb/plan/clone_expr.{h,cpp}` |
| Conjunct splitting | `qdb/plan/passes/flatten_conjucts.{h,cpp}` |

## Pipeline position

```
parse → BuildPlan → AssignSourceAliases → QualifyColumns → AnnotateTypes
      → ReorderJoins → AnnotateTypes → ExtractEquiJoins → AnnotateTypes
      → ApplyColumnPruning → physical planning
```

`BuildPlan` produces everything left of `AssignSourceAliases`. Column names are
the raw SQL identifiers (bare or `alias.col`); no types are attached.

## Operators produced

| Operator | Carries |
|---|---|
| `source` | table, alias |
| `filter` | one predicate expression |
| `project` | list of `(name, expression)` |
| `aggregate` | group keys (names), aggregate specs `(name, func, arg-expr\|null)` |
| `join` | left, right, keys `(leftCol,rightCol)`, type, residual expr (null) |

Join types: `Inner, Left, Right, Full, LeftSemi, RightSemi, LeftAnti, RightAnti`.

## Per-clause mapping

### FROM → left-deep cross-join chain

Comma-separated tables become a left-deep tree of `inner` joins with **empty
keys and no residual** (a pure cross product at build time):

```
FROM a, b, c
=>
join inner            (empty keys)
├─ join inner         (empty keys)
│  ├─ source a
│  └─ source b
└─ source c
```

`a JOIN b ON <cond>` → one `join` whose whole `ON` is the **residual** (key
extraction happens later). `USING (c)` → key `(c, c)`.

### WHERE → conjuncts, decorrelation, filter

`WHERE` is split into top-level `&&` conjuncts. Each conjunct is dispatched:

- `EXISTS / IN` (and negations) → wrap the current node in a semi/anti join
  (see [decorrelation.md](decorrelation.md)).
- a conjunct containing a scalar subquery → `ExtractScalarSubqueries` rewrites it
  and attaches the subquery (cross join or decorrelating LEFT join).
- everything else → collected into a residual list.

The residual list becomes a single `filter` on top of the (possibly
join-wrapped) node:

```
WHERE a.x = b.y AND a.z > 5
=>
filter (&& (== a.x b.y) (> a.z 5))
└─ <FROM tree>
```

### SELECT / GROUP BY / HAVING → project / aggregate

Non-aggregated query → a single `project`.

Aggregated query (`GROUP BY` present, or any aggregate call in SELECT/HAVING):

```
project   (final SELECT list; drops helper columns)
└─ filter (HAVING, if present)
   └─ aggregate keys=[<group keys>] aggs=[<specs>]
      └─ project (only when arguments must be materialized — see below)
         └─ <WHERE tree>
```

`avg(x)` has no kernel; it is rewritten to `sum(x) / cast(count(*) as f64)` —
two specs plus a projection expression.

### Aggregate argument materialization

The aggregate executor requires every aggregate **argument to be a column
reference**. When any argument is a computed expression, an inner `project`
materializes it (and passes the group keys through), and the spec's argument is
replaced by a reference:

```
sum(l_extendedprice * (1 - l_discount))  GROUP BY l_returnflag
=>
aggregate keys=[l_returnflag] aggs=[sum_0=sum(arg_0)]
└─ project (l_returnflag, arg_0 = (* l_extendedprice (- 1 l_discount)))
   └─ ...
```

Plain-column arguments (`sum(l_quantity)`) are passed through unchanged; the
inner project is then omitted unless a global key is needed (below).

### Global aggregate (no GROUP BY)

The grouping-key descriptor requires at least one key, so a constant key is
synthesized and dropped by the final project:

```
SELECT sum(l_extendedprice * l_discount)         -- no GROUP BY
=>
project (revenue)
└─ aggregate keys=[__group__] aggs=[sum_0=sum(arg_0)]
   └─ project (arg_0 = (* l_extendedprice l_discount), __group__ = (: 1 i64))
```

### count(distinct col) → double aggregation

There is no distinct kernel. `count(distinct x)` grouped by `G` is realized as a
dedup aggregate (no measures) under a counting aggregate:

```
SELECT p_brand, count(distinct ps_suppkey) GROUP BY p_brand
=>
aggregate keys=[p_brand] aggs=[count_0=count(ps_suppkey)]
└─ aggregate keys=[p_brand, ps_suppkey]            -- dedup, no measures
   └─ ...
```

Only `count(distinct <single column>)` is supported; mixing distinct and
non-distinct aggregates, or distinct over several columns, is rejected.

### SELECT DISTINCT

An `aggregate` with all output columns as group keys and no measures.

## CTE (WITH)

CTEs are **inlined per reference**: a CTE-aware source factory resolves a CTE
name by building a fresh copy of its query. Because `QualifyColumns` rewrites
identifier nodes in place, each inlined copy's expressions are deep-cloned
(`CloneOperatorExprs` → `CloneExpr`) so two copies of the same CTE do not share
mutable nodes. The optional column-alias list renames the copy's top projection
(`ApplyColumnAliases`).

```
WITH r (k, v) AS (SELECT a, sum(b) FROM t GROUP BY a)
SELECT ... FROM r, ...
=>  r is built (twice if referenced twice), its top project renamed to (k, v),
    and spliced into the FROM tree as an ordinary sub-plan.
```

`WITH RECURSIVE` is rejected.

### Derived-table column aliases

`( <subquery> ) AS d (c1, c2)` builds the subquery and renames its top
projection to `(c1, c2)` via the same `ApplyColumnAliases` helper. The derived
alias `d` itself is not applied as a qualifier (columns stay bare).

## What the builder never does

- extract equi-keys from `ON`/`WHERE` (keys stay empty / in residuals);
- push predicates onto sources or reorder joins;
- attach types or qualify columns.

These are separate, idempotent passes documented alongside.
