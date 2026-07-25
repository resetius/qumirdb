# Building the logical plan (SQL AST → operator tree)

## Directory regrouping — what moves where

The current tree groups by artifact kind, and a few things sit where they don't
logically belong (e.g. `types/` is really part of the logical-plan / data
model). Proposed concrete moves (layers, each owning everything for its stage):

| Current | New | Layer |
|---|---|---|
| `qdb/sql/` | `qdb/sql/` *(unchanged)* | Frontend |
| `qdb/sexp/` | `qdb/sexp/` *(unchanged)* | Frontend |
| `qdb/ops/` | `qdb/plan/ops/` | Logical plan |
| `qdb/pipeline/` | `qdb/plan/passes/` | Logical plan |
| `qdb/types/` | `qdb/plan/types/` | Logical plan |
| *(new)* | `qdb/plan/build.{h,cpp}` | Logical plan (SQL→plan builder) |
| `qdb/kernel/` | `qdb/kernel/` *(unchanged)* | Kernel (low-level code) |
| `qdb/exec/` | `qdb/exec/` *(unchanged)* | Execution |
| `qdb/modules/` | `qdb/modules/` *(unchanged)* | Runtime |
| `qdb/io/` | `qdb/io/` *(unchanged)* | Support |
| `qdb/utils/` | `qdb/utils/` *(unchanged)* | Support |

So three directories move under a new `qdb/plan/` umbrella (`ops`, `pipeline →
passes`, `types`) and the new builder joins them; everything else keeps its
place. `kernel/` stays as the low-level stage (gen, JIT, key descriptors,
`.oz`). The regrouping is mechanical and independent of the builder work —
until it happens the builder can live at `qdb/sql/build.{h,cpp}`.

### Stage 0 — regrouping (done)

- [x] `git mv qdb/ops qdb/plan/ops`
- [x] `git mv qdb/pipeline qdb/plan/passes`
- [x] `git mv qdb/types qdb/plan/types`
- [x] fix `#include` paths (`qdb/ops/…` → `qdb/plan/ops/…`, etc.)
- [x] update source lists in `qdb/CMakeLists.txt`
- [x] rebuild + `ctest` green

---

The rest of this document describes how the SQL frontend (`qdb/sql/*`) will be
turned into the logical operator tree (`qdb/ops/*`) consumed by the existing
passes (qualify → typing → pruning) and the physical planner.

## Where plan building fits

```
SQL text ──lexer/parser──▶ TSqlNode AST ──[BUILD PLAN]──▶ logical plan (IOperator tree)
                                                              │
                                              AssignSourceAliases
                                              QualifyColumns
                                              AnnotateTypes
                                              ApplyColumnPruning
                                                              ▼
                                                  TPhysicalPlanner::Build ──▶ runtime
```

Two facts make the builder small:

1. **Expressions are already shared.** The SQL parser produces
   `NQumir::NAst::TExprPtr`, the exact type the operators store
   (`TFilterOperator::Predicate_`, `TProjectionSpec::Expression`,
   `TAggregateSpec::Arg`, `TJoinOperator::Filter_`). Scalar expressions move
   into the plan **as-is** — no translation.
2. **Names and types are someone else's job.** `QualifyColumns` resolves bare
   identifiers to `alias.col`, and `AnnotateTypes` assigns schemas. The builder
   therefore emits an *unqualified, untyped* plan — exactly like the sexp
   parser (`qdb/sexp/parser.cpp`) does today.

So building the plan is purely *structural*: turn SELECT/FROM/WHERE/GROUP
BY/JOIN into a source/join/filter/project/aggregate tree.

## Module API

```cpp
// qdb/plan/build.h   (or qdb/sql/build.h until the regrouping)
namespace NQqb {

// Resolves a table name to a source operator (catalog lookup), analogous to
// NSexp::TRelParserOptions::SourceFactory.
using TTableSourceFactory =
    std::function<std::expected<TOperatorPtr, NQumir::TError>(std::string_view table)>;

std::expected<TOperatorPtr, NQumir::TError>
BuildPlan(const NSql::TSqlNodePtr& query, const TTableSourceFactory& sources);

} // namespace NQqb
```

Output is a raw `IOperator` tree; callers then run the existing passes and
`TPhysicalPlanner`.

## Build algorithm (per clause)

Driven by `TSqlQuery` → `TSqlSelect`. Build bottom-up:

The builder is a *naive, mechanical* translation. Plan-shape rewrites that exist
for efficiency — equi-join extraction, predicate pushdown, join ordering — are
**not** done here; they are optimizer passes (see "Optimizer passes" below).

### 1. FROM → source / join tree
- `TSqlTableName` → `sources(name)` → `TSourceOperator` (alias respected).
- `TSqlSubqueryTable` → recursive `BuildPlan`; column-alias list becomes a thin
  `project` rename on top.
- explicit `TSqlJoin` (`JOIN … ON`/`USING`) → `TJoinOperator`. Keys come from
  the `ON` equalities (or `USING`); non-equi `ON` conjuncts become the join
  **residual** filter. (Syntactic — the user wrote the join.)
- comma-FROM `FROM a, b, c` and `CROSS JOIN` → a **cross-join** (cartesian)
  tree, left-deep in FROM order. Turning a cross join + filter into equijoins is
  an optimizer pass, not the builder's job. *(Prerequisite: the plan has no
  cross-join representation yet — see Gaps.)*

### 2. WHERE → filter
The builder emits the remaining WHERE (everything not already consumed by an
explicit `JOIN … ON` or by EXISTS/IN decorrelation) as a **single top-level
`TFilterOperator`** above the FROM tree. Splitting the conjunction, pushing
single-relation predicates toward their sources, and promoting cross-relation
equalities to join keys are all optimizer passes.

### 3. SELECT / GROUP BY / HAVING → project / aggregate
- Detect aggregates by scanning the select list and HAVING for calls to
  `sum/count/avg/min/max` (the `TAggregateSpec::Func` set).
- **No aggregates, no GROUP BY** → a single `TProjectOperator` from the select
  list (`*` / `table.*` expand against the input schema).
- **Aggregates or GROUP BY present**:
  1. `TAggregateOperator::Arg` accepts an arbitrary expression, so aggregate
     **arguments go in directly** — e.g. `sum(l_extendedprice * (1 - l_discount))`
     becomes one `TAggregateSpec` with that expression as `Arg`. The inner
     `project` in `q1.sexp` that materializes `disc_price` once and reuses it is
     common-subexpression elimination — an **optimizer pass**, not the builder.
  2. `TAggregateOperator::GroupKeys` are column *names*, so a `GROUP BY` on an
     *expression* needs a pre-`project` to materialize it into a named column
     (necessary for representability — group keys can't hold an expression).
     `GROUP BY <column>` (the TPC-H case) needs no pre-project.
  3. emit `TAggregateOperator(keys, aggs)`.
  4. emit a final `project` for the output select list, rewriting aggregate
     calls to references of the aggregate's output columns.
  - `avg(x)` → `sum(x)` + `count(*)` then `sum / cast(count f64)` in the final
    project — necessary (there is no `avg` aggregate func), not an optimization.
  - `count(distinct x)` (parsed as `count(distinct(x))`) → double aggregation:
    inner `aggregate` grouping by `x`, outer `count` — necessary (no DISTINCT
    aggregate).
- **HAVING** → `TFilterOperator` placed *after* the aggregate.

### 4. SELECT DISTINCT
`aggregate` with all output columns as group keys and no aggregates.

### 5. ORDER BY / LIMIT / OFFSET
**Blocked: no `sort` / `limit` operators exist.** Requires new logical
operators `TSortOperator(keys, directions, nulls)` and
`TLimitOperator(limit, offset)` plus their physical executors. Until then,
queries with `ORDER BY` / `LIMIT` build their body and drop the tail (or error).

### 6. WITH (CTE)
Resolve each CTE name to its built sub-plan in a scope; FROM references
substitute the sub-plan (inline). Recursive CTEs are out of scope.

### 7. Subquery expressions (EXISTS / IN / scalar)
- **EXISTS / NOT EXISTS** (top-level `AND` conjunct of WHERE) decorrelate into a
  semi / anti join — no new plan node needed (see §1 / Phase 2):
  ```
  FROM <outer>
  WHERE EXISTS (SELECT … FROM <inner> WHERE <corr> AND <local>)  [AND <rest>]
     →  <outer> LEFT SEMI JOIN (filter <inner> by <local>) ON <corr-equi>
  ```
  `NOT EXISTS` → `LEFT ANTI JOIN`. Equi correlation predicates become join keys;
  non-equi parts become the join **residual** filter (`join_exec` supports
  residual semi/anti). `LeftSemi`/`LeftAnti` are implemented in both the logical
  operator and the executor.
- **IN / NOT IN (subquery)** map the same way: `x IN (SELECT y …)` → semi join on
  `x = y`; `NOT IN` → anti join. ⚠ `NOT IN` has three-valued NULL semantics — an
  anti join is **not** equivalent when `y` can be NULL; guard or restrict.
- Preconditions: the EXISTS/IN must be an `AND`-conjunct (not under `OR`, not in
  the select list). Otherwise it cannot become a join.
- **Scalar subqueries** (`= (SELECT …)`, subqueries in the projection) are *not*
  expressible as joins and still need a dedicated mechanism. **This is the only
  remaining blocked subquery case.**

## Optimizer passes (separate from the builder)

The builder emits a correct but naive plan; these are distinct rewrite passes
over the operator tree (home: `qdb/plan/passes/`), run after building:

- **Equi-join extraction** — rewrite `cross-join + filter` into equijoins:
  pull `col = col` conjuncts that span two relations up as join keys, leave the
  rest as the join residual / a smaller filter. Includes join ordering
  (start left-deep following FROM; cost-based later).
- **Predicate pushdown** — move single-relation conjuncts of a filter down
  toward their source.
- **Column pruning** — already exists (`ApplyColumnPruning`).

## Gaps to close (tracked)

| Gap | Needed for |
|---|---|
| Cross-join (cartesian) representation in the plan | building comma-FROM / `CROSS JOIN` (today join keys must be non-empty; no `Cross` type) |
| `sort` / `limit` logical + physical operators | ORDER BY, LIMIT, OFFSET |
| Scalar-subquery mechanism | `= (SELECT …)` and subqueries in the select list |
| `*` / `table.*` expansion | star projections |

Not gaps: equi-join extraction and predicate pushdown are **optimizer passes**,
not builder work; EXISTS/NOT EXISTS and IN/NOT IN (subquery) decorrelate into
existing semi / anti joins.

## Phased implementation

### Phase 0 — scaffolding
- [x] add `qdb/plan/build.{h,cpp}` with `BuildPlan` + `TTableSourceFactory`
- [x] wire into `qdb/CMakeLists.txt`

### Phase 1 — single-relation core (covers q1, q6)
- [x] `FROM t` → `TSourceOperator` via the source factory
- [x] `WHERE` → `TFilterOperator`
- [x] plain `SELECT` (no aggregates) → `TProjectOperator`
- [x] aggregate detection (`sum/count/avg/min/max`) over select list + HAVING
- [x] aggregate args go directly into `TAggregateSpec::Arg` → `TAggregateOperator` → final `project`
- [x] `avg(x)` → `sum(x)` + `count(*)`, divided in the final project
- [ ] `count(distinct x)` → double aggregation
- [x] `HAVING` → `TFilterOperator` after the aggregate
- [ ] `SELECT DISTINCT` → `aggregate` keyed by all output columns
- [ ] `GROUP BY <expression>` → pre-`project` (group keys are column names)

### Phase 1-test — parser + plan-builder regression
- [x] extend `test/test_reg.cpp`: parse → `BuildPlan` → print via `NSexp::MakeRelPrinters()`
- [x] canonize **naive** plans as `*.plan` goldens (no optimizer passes applied)
- [x] canonize the Phase 1 cases (q1, q6, and the single-relation `cases/**`)

### Phase 2 — joins (build only; incl. EXISTS/IN decorrelation)
- [x] explicit `JOIN … ON` → `TJoinOperator` with the whole `ON` as residual (key extraction is an optimizer pass)
- [x] explicit `JOIN … USING` → `TJoinOperator` keys `(col, col)`
- [x] comma-FROM / `CROSS JOIN` → left-deep cross-join (cartesian) tree, `WHERE` → one top-level filter
- [x] derived tables `( select ) [AS a]` → recursive build (column-alias list deferred → q13)
- [x] `EXISTS` / `NOT EXISTS` conjunct → `LEFT SEMI` / `LEFT ANTI` join (whole subquery WHERE → residual)
- [x] `IN` / `NOT IN (subquery)` → semi / anti join on `operand == <subquery column>` (⚠ `NOT IN` NULL semantics not yet guarded)
- [x] goldens for the join queries (q3, q4, q5, q7, q8, q9, q10, q12, q14, q18, q19, q21)

### Phase 2-opt — optimizer passes (separate from the builder)
- [ ] equi-join extraction: `cross-join + filter` → equijoins, key selection, left-deep order
- [ ] predicate pushdown: single-relation conjuncts → toward sources
- [ ] (common-subexpression elimination for shared agg args — later)

### Phase 3 — sort / limit operators
- [ ] `TSortOperator(keys, directions, nulls)` logical + physical executor
- [ ] `TLimitOperator(limit, offset)` logical + physical executor
- [ ] `ORDER BY` / `LIMIT` / `OFFSET` → these operators
- [ ] goldens refreshed with the tail

### Phase 4 — CTE & scalar subqueries
- [ ] `WITH` CTE inlining (name scope, FROM substitution)
- [ ] scalar-subquery mechanism (`= (SELECT …)`, subqueries in the select list)
- [ ] goldens for q2, q11, q15, q17, …

## Testing

The `test_reg.cpp` plan mode (added in Phase 1-test) canonizes the **naive**
builder output (no optimizer passes) as `*.plan` goldens per `cases/**.sql`,
growing as phases land. Optimizer passes get their own goldens later. The
hand-written `benchmark/tpch/queries/*.sexp` plans encode manual optimizations —
use them as the target *shape* to converge toward, not a byte-for-byte golden.

## Source map

| Area | Source |
|---|---|
| SQL AST | `qdb/sql/ast.h` |
| SQL parser | `qdb/sql/parser.cpp` |
| Logical operators | `qdb/ops/*.{h,cpp}` (→ `plan/ops/`) |
| Operator base / `TMaybeOp` | `qdb/ops/operator.h` |
| Logical passes | `qdb/pipeline/*` (→ `plan/passes/`) |
| sexp plan parser/printer (reference) | `qdb/sexp/*` |
| Physical planner | `qdb/exec/planner.cpp` |
| New SQL→plan builder (to add) | `qdb/plan/build.{h,cpp}` (or `qdb/sql/build.*`) |
