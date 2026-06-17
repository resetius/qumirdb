(rel project
  (rel aggregate
    (rel project
      (rel filter
        (rel source "__LINEITEM__")
        (: (&& (&& (&& (>= l_shipdate (: __Q6_DATE_LOW__ i32))
                       (< l_shipdate  (: __Q6_DATE_HIGH__ i32)))
                   (&& (>= l_discount (: __Q6_DISC_LOW__ f64))
                       (<= l_discount (: __Q6_DISC_HIGH__ f64))))
               (< l_quantity (: __Q6_QUANTITY__ f64))) u8))
      (rev   (* l_extendedprice l_discount))
      (dummy (: 1 i64)))
    (keys dummy)
    (agg revenue sum rev))
  (revenue revenue))
