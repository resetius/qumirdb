(rel aggregate
  (rel project
    (rel join
      (rel join
        (rel join
          (rel join
            (rel source "__LINEITEM__")
            (rel source "__ORDERS__") ((l_orderkey o_orderkey)) (inner))
          (rel source "__CUSTOMER__") ((o_custkey c_custkey)) (inner))
        (rel source "__PART__") ((l_partkey p_partkey)) (inner))
      (rel source "__SUPPLIER__") ((l_suppkey s_suppkey)) (inner))
    (volume (* l_extendedprice (- (: 1.0 f64) l_discount)))
    (o_orderdate o_orderdate))
  (keys o_orderdate)
  (agg total_volume sum volume))
