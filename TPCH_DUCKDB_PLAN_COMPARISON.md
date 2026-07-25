# TPC-H Plan Comparison: qdb vs DuckDB

DuckDB plans were dumped with:

```bash
~/Projects/tpch/dump_duckdb_explain.sh ~/Projects/tpch 1
```

The runner writes plans to `build/duckdb-explain/sf1/qN.explain.txt`. It normalizes qgen SQL only for `EXPLAIN`: Oracle-style `rownum` tails become `LIMIT` or are removed, and `interval 'N' day (3)` becomes DuckDB-compatible interval syntax.

| Q | Push-down: qdb vs DuckDB | Join order / form |
|---|---|---|
| Q1 | Same: `lineitem` date is pushed. | No joins; DuckDB keeps `ORDER_BY`. |
| Q2 | Same: `part` and `region` predicates are pushed. | DuckDB uses `LEFT_DELIM_JOIN` for scalar minimum; qdb uses explicit duplicated subquery structure. |
| Q3 | Same: `customer`, `orders`, and `lineitem` filters are pushed. | Similar join order; DuckDB adds `TOP_N 10`. |
| Q4 | Same. | Both model EXISTS as semi join; DuckDB uses delimiter/semi join machinery. |
| Q5 | Same for `region` and `orders` filters. | Same join graph; DuckDB re-associates hash join branches. |
| Q6 | Same: all filters are on `lineitem`. | No joins. |
| Q7 | Same: date and nation filters are pushed; final nation-pair predicate stays after joins. | Same graph; DuckDB physically reorders hash branches. |
| Q8 | Same. | Same graph; DuckDB physically reorders hash branches. |
| Q9 | Same: `part.p_name contains(...)` is pushed. | Different: DuckDB builds two branches, `lineitem join filtered part join orders` and `partsupp join supplier join nation`, then joins them by `(partkey, suppkey)`. qdb currently uses left-deep `filtered part -> partsupp -> lineitem -> supplier -> orders -> nation`. |
| Q10 | Same. | Similar join order; DuckDB adds `TOP_N 20`. |
| Q11 | Same: nation filter is pushed in both branches. | DuckDB gets common-subplan-like reuse; qdb duplicates aggregate branches. |
| Q12 | Same or better after optimization: DuckDB pushes receipt date to scan and keeps other lineitem predicates before join. | Same order: `orders join filtered lineitem`. |
| Q13 | Same: orders comment predicate is pushed. | Similar left outer join shape. |
| Q14 | Same: lineitem shipdate is pushed. | Same order. |
| Q15 | Same: lineitem date is pushed. | DuckDB extracts revenue aggregate as a common subplan/CTE; qdb currently expresses separate branches. |
| Q16 | Same. | Similar graph; DuckDB uses anti/mark machinery. |
| Q17 | Same. | DuckDB decorrelates through delimiter/semi joins; qdb has explicit semi plus threshold join. |
| Q18 | Same. | Similar to qdb: threshold aggregate -> semi orders -> customer -> full lineitem for final sum; DuckDB adds `TOP_N 100`. |
| Q19 | qdb is better: qdb pushes common `part` brand/size and `lineitem` filters; DuckDB leaves the large OR after the join except for some lineitem scan filters. | Same `part join lineitem` graph; qdb should feed fewer rows into the join. |
| Q20 | Same: part prefix, nation, and lineitem date are pushed. | Different: DuckDB builds qualifying partsupp through delimiter/semi joins, then uses `RIGHT_SEMI` against supplier+nation; qdb starts supplier+nation earlier. |
| Q21 | Same basic filters: nation, order status, and l1/l3 receipt-vs-commit predicates are pushed. | DuckDB uses `RIGHT_DELIM_JOIN` semi/anti; qdb uses explicit inner orders before semi/anti. The likely hotspot is residual semi/anti over lineitem, not simple push-down. |
| Q22 | Same: phone prefix and account balance predicates are pushed. | Similar logic; DuckDB uses scalar aggregate plus right anti join. |

## Main Takeaways

- Q9 is the strongest DuckDB reference for join-order work. DuckDB avoids the qdb/YQL left-deep chain and uses a bushy plan with a final composite join on `(partkey, suppkey)`.
- Q19 is not a DuckDB pattern to copy: qdb has stronger predicate decomposition before the join.
- Q15 and Q11 show potential value from common subplan reuse, but that is separate from join ordering.
- Q21 mostly points at semi/anti residual implementation and cardinality handling rather than missing predicate push-down.
