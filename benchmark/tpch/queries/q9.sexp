(rel aggregate
  (rel project
    (rel join
      (rel join
        (rel join
          (rel join
            (rel source "__PART__")
            (rel source "__PARTSUPP__") ((p_partkey ps_partkey)) (inner))
          (rel source "__LINEITEM__") ((ps_partkey l_partkey) (ps_suppkey l_suppkey)) (inner))
        (rel source "__SUPPLIER__") ((l_suppkey s_suppkey)) (inner))
      (rel source "__NATION__") ((s_nationkey n_nationkey)) (inner))
    (amount (- (* l_extendedprice (- (: 1.0 f64) l_discount)) (* ps_supplycost l_quantity)))
    (n_name n_name)
    (l_shipdate l_shipdate))
  (keys n_name l_shipdate)
  (agg sum_profit sum amount))
