(rel aggregate
  (rel aggregate
    (rel join
      (rel source "__CUSTOMER__")
      (rel source "__ORDERS__") ((c_custkey o_custkey)) (left))
    (keys c_custkey)
    (agg c_count count))
  (keys c_count)
  (agg custdist count))
