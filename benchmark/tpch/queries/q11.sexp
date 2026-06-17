(rel aggregate
  (rel project
    (rel join
      (rel join
        (rel source "__PARTSUPP__")
        (rel source "__SUPPLIER__") ((ps_suppkey s_suppkey)) (inner))
      (rel filter (rel source "__NATION__") (: (== n_name "__Q11_NATION__") u8))
      ((s_nationkey n_nationkey)) (inner))
    (ps_partkey ps_partkey)
    (value (* ps_supplycost ps_availqty))
    (dummy (: 1 i64)))
  (keys ps_partkey)
  (agg value sum value))
