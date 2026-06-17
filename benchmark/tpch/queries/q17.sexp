(rel project
  (rel aggregate
    (rel project
      (rel join
        (rel filter (rel source "__LINEITEM__") (: (: 1 i64) u8))
        (rel filter (rel source "__PART__")
          (: (&& (== p_brand "__Q17_BRAND__") (== p_container "__Q17_CONTAINER__")) u8))
        ((l_partkey p_partkey)) (inner))
      (l_extendedprice l_extendedprice)
      (dummy (: 1 i64)))
    (keys dummy)
    (agg sum_price sum l_extendedprice))
  (avg_yearly (/ sum_price (: 7.0 f64))))
