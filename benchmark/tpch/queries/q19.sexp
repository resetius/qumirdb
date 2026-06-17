(rel project
  (rel aggregate
    (rel project
      (rel join
        (rel source "__LINEITEM__")
        (rel source "__PART__") ((l_partkey p_partkey)) (inner))
      (disc_price (* l_extendedprice (- (: 1.0 f64) l_discount)))
      (dummy (: 1 i64)))
    (keys dummy)
    (agg revenue sum disc_price))
  (revenue revenue))
