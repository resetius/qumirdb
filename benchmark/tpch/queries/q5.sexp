(rel aggregate
  (rel project
    (rel join
      (rel join
        (rel join
          (rel join
            (rel join
              (rel filter (rel source "__CUSTOMER__") (: (: 1 i64) u8))
              (rel filter (rel source "__ORDERS__")
                (: (&& (>= o_orderdate (: __Q5_DATE_LOW__ i32)) (< o_orderdate (: __Q5_DATE_HIGH__ i32))) u8))
              ((c_custkey o_custkey)) (inner))
            (rel source "__LINEITEM__") ((o_orderkey l_orderkey)) (inner))
          (rel filter (rel source "__SUPPLIER__") (: (: 1 i64) u8))
          ((l_suppkey s_suppkey)) (inner))
        (rel filter (rel source "__NATION__") (: (: 1 i64) u8))
        ((s_nationkey n_nationkey)) (inner))
      (rel filter (rel source "__REGION__") (: (== r_name "__Q5_REGION__") u8))
      ((n_regionkey r_regionkey)) (inner))
    (n_name n_name)
    (disc_price (* l_extendedprice (- (: 1.0 f64) l_discount))))
  (keys n_name)
  (agg revenue sum disc_price))
