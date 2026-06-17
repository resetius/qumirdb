(rel filter
  (rel project
    (rel join
      (rel aggregate
        (rel filter (rel source "__LINEITEM__")
          (: (&& (>= l_shipdate (: __Q15_DATE_LOW__ i32)) (< l_shipdate (: __Q15_DATE_HIGH__ i32))) u8))
        (keys l_suppkey)
        (agg total_revenue sum l_extendedprice))
      (rel source "__SUPPLIER__") ((l_suppkey s_suppkey)) (inner))
    (s_suppkey     s_suppkey)
    (s_name        s_name)
    (s_address     s_address)
    (s_phone       s_phone)
    (total_revenue total_revenue))
  (: (== total_revenue (scalar_subquery_max_revenue)) u8))
