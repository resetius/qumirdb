(rel aggregate
  (rel project
    (rel join
      (rel join
        (rel join
          (rel source "__CUSTOMER__")
          (rel filter (rel source "__ORDERS__")
            (: (&& (>= o_orderdate (: __Q10_DATE_LOW__ i32)) (< o_orderdate (: __Q10_DATE_HIGH__ i32))) u8))
          ((c_custkey o_custkey)) (inner))
        (rel filter (rel source "__LINEITEM__") (: (== l_returnflag "R") u8))
        ((o_orderkey l_orderkey)) (inner))
      (rel source "__NATION__") ((c_nationkey n_nationkey)) (inner))
    (c_custkey      c_custkey)
    (c_name         c_name)
    (c_acctbal      c_acctbal)
    (c_phone        c_phone)
    (c_address      c_address)
    (c_comment      c_comment)
    (n_name         n_name)
    (disc_price (* l_extendedprice (- (: 1.0 f64) l_discount))))
  (keys c_custkey c_name c_acctbal c_phone n_name c_address c_comment)
  (agg revenue sum disc_price))
