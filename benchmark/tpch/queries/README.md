# TPC-H benchmark queries

`run_tpch.sh` runs the queries in two modes (`MODE=sexpr` default, `MODE=sql`),
both parametrized per scale from `../params/sf<N>.sh`:

- **sexpr** — hand-written logical plans in [`sexpr/`](sexpr/). Table paths and
  scale constants are placeholders (`__LINEITEM__`, `__Q3_DATE__` as day-int, …).

- **sql** — SQL templates in [`sql/`](sql/), adapted from `~/Projects/tpch/sql*`
  into the qdb dialect. Tables are referenced by name (resolved from the parquet
  directory via `--data`); scale constants are placeholders — strings/numbers
  shared with sexpr (`__Q3_SEGMENT__`, …) plus date-literal placeholders
  (`__Q3_DATE_S__`, `__Q1_DELTA__`) whose values live in the same `params` files.

Constants that the sexpr plans do not parametrize (e.g. q9 `p_name` colour,
q19 brands) are left hard-coded at their sf1 values in both query sets.
