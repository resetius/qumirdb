(rel project
  (rel aggregate
    (rel project
      (rel filter
        (rel join
          (rel join
            (rel source "__LINEITEM__")
            (rel filter (rel source "__PART__")
              (: (&& (== p_brand "__Q17_BRAND__") (== p_container "__Q17_CONTAINER__")) u8))
            ((l_partkey p_partkey)) (inner))
          (rel project
            (rel aggregate
              (rel join
                (rel source "__LINEITEM__")
                (rel filter (rel source "__PART__")
                  (: (&& (== p_brand "__Q17_BRAND__") (== p_container "__Q17_CONTAINER__")) u8))
                ((l_partkey p_partkey)) (left_semi))
              (keys l_partkey)
              (agg sum_qty   sum   l_quantity)
              (agg cnt_qty   count l_quantity))
            (threshold_partkey l_partkey)
            (threshold         (* (/ sum_qty (cast cnt_qty f64)) (: 0.2 f64))))
          ((l_partkey threshold_partkey)) (inner))
        (: (< l_quantity threshold) u8))
      (l_extendedprice l_extendedprice)
      (dummy           (: 1 i64)))
    (keys dummy)
    (agg sum_price sum l_extendedprice))
  (avg_yearly (/ sum_price (: 7.0 f64))))
