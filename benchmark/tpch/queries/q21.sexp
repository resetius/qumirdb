(rel aggregate
  (rel join
    (rel join
      (rel join
        (rel join
          (rel source "__SUPPLIER__")
          (rel filter
            (rel source "__LINEITEM__")
            (: (> l_receiptdate l_commitdate) u8))
          ((s_suppkey l_suppkey)) (inner))
        (rel filter (rel source "__ORDERS__") (: (== o_orderstatus "F") u8))
        ((l_orderkey o_orderkey)) (inner))
      (rel filter (rel source "__NATION__") (: (== n_name "__Q21_NATION__") u8))
      ((s_nationkey n_nationkey)) (inner))
    (rel source "__LINEITEM__") ((l_orderkey l_orderkey)) (left_semi))
  (keys s_name)
  (agg numwait count))
