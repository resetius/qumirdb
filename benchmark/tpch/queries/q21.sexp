(rel aggregate
  (rel join
    (rel join
      (rel join
        (rel join
          (rel join
            (rel source "__SUPPLIER__")
            (rel filter
              (rel source "__LINEITEM__" "l1")
              (: (> l_receiptdate l_commitdate) u8))
            ((s_suppkey l_suppkey)) (inner))
          (rel filter (rel source "__ORDERS__") (: (== o_orderstatus "F") u8))
          ((l_orderkey o_orderkey)) (inner))
        (rel filter (rel source "__NATION__") (: (== n_name "__Q21_NATION__") u8))
        ((s_nationkey n_nationkey)) (inner))
      (rel source "__LINEITEM__" "l2")
      ((l_orderkey l_orderkey)) (left_semi) (!= |l1.l_suppkey| |l2.l_suppkey|))
    (rel filter
      (rel source "__LINEITEM__" "l3")
      (: (> l_receiptdate l_commitdate) u8))
    ((|l1.l_orderkey| |l3.l_orderkey|)) (left_anti) (!= |l1.l_suppkey| |l3.l_suppkey|))
  (keys s_name)
  (agg numwait count))
