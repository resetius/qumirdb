(rel sort
  (rel aggregate
    (rel project
      (rel filter
        (rel join
          (rel join
            (rel join
              (rel join
                (rel join
                  (rel source "__NATION__")
                  (rel filter (rel source "__REGION__") (: (== r_name "__Q5_REGION__") u8))
                  ((n_regionkey r_regionkey)) (inner))
                (rel source "__SUPPLIER__")
                ((n_nationkey s_nationkey)) (inner))
              (rel source "__LINEITEM__")
              ((s_suppkey l_suppkey)) (inner))
            (rel filter (rel source "__ORDERS__")
              (: (&& (>= o_orderdate (: __Q5_DATE_LOW__ i32)) (< o_orderdate (: __Q5_DATE_HIGH__ i32))) u8))
            ((l_orderkey o_orderkey)) (inner))
          (rel source "__CUSTOMER__")
          ((o_custkey c_custkey)) (inner))
        (: (== c_nationkey s_nationkey) u8))
      (n_name n_name)
      (disc_price (* l_extendedprice (- (: 1.0 f64) l_discount))))
    (keys n_name)
    (agg revenue sum disc_price))
  (revenue desc nulls-default))
