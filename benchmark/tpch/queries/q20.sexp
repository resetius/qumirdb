(rel project
  (rel join
    (rel join
      (rel source "__SUPPLIER__")
      (rel filter (rel source "__NATION__") (: (== n_name "__Q20_NATION__") u8))
      ((s_nationkey n_nationkey)) (inner))
    (rel join
      (rel source "__PARTSUPP__")
      (rel filter
        (rel source "__LINEITEM__")
        (: (&& (>= l_shipdate (: __Q20_DATE_LOW__ i32))
               (< l_shipdate  (: __Q20_DATE_HIGH__ i32))) u8))
      ((ps_partkey l_partkey)) (left_semi))
    ((s_suppkey ps_suppkey)) (left_semi))
  (s_name s_name)
  (s_address s_address))
