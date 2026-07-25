# TPC-H Query Status

Queries live in `benchmark/tpch/queries/`. Dates stored as `i32` (days since Unix epoch).

## Working (22 / 22)

| Query | Notes |
|-------|-------|
| Q1  | Pricing Summary — full aggregation with avgs |
| Q2  | Minimum Cost Supplier — grouped min + inner join, eliminates scalar subquery |
| Q3  | Shipping Priority — 3-way join, revenue aggregate |
| Q4  | Order Priority — semi-join (EXISTS via left_semi) |
| Q5  | Local Supplier Volume — incl. `c_nationkey = s_nationkey` filter |
| Q6  | Forecasting Revenue — simple filter + sum |
| Q7  | Volume Shipping — `qdb_date_year` in project, 2-nation filter |
| Q8  | National Market Share — `qdb_date_year`, PROMO filter, if/sum mkt_share |
| Q9  | Product Type Profit — `qdb_date_year`, LIKE 'turquoise%' on part |
| Q10 | Returned Items — 4-way join incl. NATION |
| Q11 | National Market Share of Supplier (Part) — grouped sum + cross join global threshold |
| Q12 | Shipping Modes — CASE-style if/sum |
| Q13 | Customer Distribution — NOT LIKE on o_comment before LEFT JOIN |
| Q14 | Promotion Effect — LIKE 'PROMO%' via `qdb_string_view_sql_like` |
| Q15 | Top Supplier — grouped sum + cross join global max |
| Q16 | Parts/Supplier Relationship — COUNT DISTINCT via double aggregation |
| Q17 | Small-Quantity-Order Revenue — grouped avg (sum/count) + inner join |
| Q18 | Large Volume Customer — semi-join with pre-aggregated lineitem HAVING |
| Q19 | Discounted Revenue — full 3-way OR filter on brand/container/qty/size |
| Q20 | Potential Part Promotion — grouped sum + 2-key inner join + nation filter |
| Q21 | Suppliers Who Kept Orders Waiting — LeftSemi + LeftAnti with residual inequality filter |
| Q22 | Global Sales Opportunity — cross join global avg, `qdb_substring` in project + filter |
