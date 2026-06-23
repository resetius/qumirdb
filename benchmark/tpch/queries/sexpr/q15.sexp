(rel project
  (rel filter
    (rel join
      (rel join
        (rel aggregate
          (rel filter (rel source "__LINEITEM__")
            (: (&& (>= l_shipdate (: __Q15_DATE_LOW__ i32))
                   (< l_shipdate (: __Q15_DATE_HIGH__ i32))) u8))
          (keys l_suppkey)
          (agg total_revenue sum l_extendedprice))
        (rel source "__SUPPLIER__") ((l_suppkey s_suppkey)) (inner))
      (rel project
        (rel aggregate
          (rel project
            (rel aggregate
              (rel filter (rel source "__LINEITEM__")
                (: (&& (>= l_shipdate (: __Q15_DATE_LOW__ i32))
                       (< l_shipdate (: __Q15_DATE_HIGH__ i32))) u8))
              (keys l_suppkey)
              (agg total_revenue sum l_extendedprice))
            (total_revenue total_revenue)
            (dummy (: 1 i64)))
          (keys dummy)
          (agg max_revenue max total_revenue))
        (max_revenue max_revenue))
      () (inner))
    (: (== total_revenue max_revenue) u8))
  (s_suppkey     s_suppkey)
  (s_name        s_name)
  (s_address     s_address)
  (s_phone       s_phone)
  (total_revenue total_revenue))
