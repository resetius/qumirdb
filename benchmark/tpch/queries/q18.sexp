(rel aggregate
  (rel join
    (rel join
      (rel source "__CUSTOMER__")
      (rel join
        (rel source "__ORDERS__")
        (rel filter
          (rel aggregate
            (rel source "__LINEITEM__" "l2")
            (keys l_orderkey)
            (agg sum_qty sum l_quantity))
          (: (> sum_qty (: 315.0 f64)) u8))
        ((o_orderkey l_orderkey)) (left_semi))
      ((c_custkey o_custkey)) (inner))
    (rel source "__LINEITEM__") ((o_orderkey l_orderkey)) (inner))
  (keys c_name c_custkey o_orderkey o_orderdate o_totalprice)
  (agg sum_qty sum l_quantity))
