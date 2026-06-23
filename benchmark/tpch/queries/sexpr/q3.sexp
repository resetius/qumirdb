(rel aggregate
  (rel project
    (rel join
      (rel join
        (rel filter (rel source "__CUSTOMER__") (: (== c_mktsegment "__Q3_SEGMENT__") u8))
        (rel filter (rel source "__ORDERS__") (: (< o_orderdate (: __Q3_DATE__ i32)) u8))
        ((c_custkey o_custkey)) (inner))
      (rel filter (rel source "__LINEITEM__") (: (> l_shipdate (: __Q3_DATE__ i32)) u8))
      ((o_orderkey l_orderkey)) (inner))
    (l_orderkey      l_orderkey)
    (o_orderdate     o_orderdate)
    (o_shippriority  o_shippriority)
    (disc_price (* l_extendedprice (- (: 1.0 f64) l_discount))))
  (keys l_orderkey o_orderdate o_shippriority)
  (agg revenue sum disc_price))
