(rel project
  (rel aggregate
    (rel project
      (rel filter
        (rel join
          (rel source "__LINEITEM__")
          (rel filter (rel source "__PART__")
            (: (&& (== p_brand "__Q17_BRAND__") (== p_container "__Q17_CONTAINER__")) u8))
          ((l_partkey p_partkey)) (inner))
        (: (< l_quantity (scalar_subquery_avg_qty p_partkey)) u8))
      (l_extendedprice l_extendedprice)
      (dummy (: 1 i64)))
    (keys dummy)
    (agg sum_price sum l_extendedprice))
  (avg_yearly (/ sum_price (: 7.0 f64))))
