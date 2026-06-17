(rel filter
  (rel aggregate
    (rel project
      (rel join
        (rel join
          (rel source "__SUPPLIER__")
          (rel filter (rel source "__NATION__") (: (== n_name "__Q11_NATION__") u8))
          ((s_nationkey n_nationkey)) (inner))
        (rel source "__PARTSUPP__")
        ((s_suppkey ps_suppkey)) (inner))
      (ps_partkey ps_partkey)
      (value (* ps_supplycost ps_availqty))
      (dummy (: 1 i64)))
    (keys ps_partkey)
    (agg value sum value))
  (: (> value (scalar_subquery_nation_total "__Q11_NATION__")) u8))
