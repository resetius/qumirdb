(rel aggregate
  (rel join
    (rel join
      (rel source "__CUSTOMER__")
      (rel source "__ORDERS__") ((c_custkey o_custkey)) (inner))
    (rel source "__LINEITEM__") ((o_orderkey l_orderkey)) (inner))
  (keys c_name c_custkey o_orderkey o_orderdate o_totalprice)
  (agg sum_qty sum l_quantity))
