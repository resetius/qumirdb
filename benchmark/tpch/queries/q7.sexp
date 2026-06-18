(rel aggregate
  (rel project
    (rel filter
      (rel join
        (rel join
          (rel filter
            (rel source "__LINEITEM__")
            (: (&& (>= l_shipdate (: 9131 i32))
                   (<= l_shipdate (: 9861 i32))) u8))
          (rel join
            (rel source "__ORDERS__")
            (rel join
              (rel source "__CUSTOMER__")
              (rel filter (rel source "__NATION__" "n2")
                (: (|| (== n_name "__Q7_NATION1__") (== n_name "__Q7_NATION2__")) u8))
              ((c_nationkey n_nationkey)) (inner))
            ((o_custkey c_custkey)) (inner))
          ((l_orderkey o_orderkey)) (inner))
        (rel join
          (rel source "__SUPPLIER__")
          (rel filter (rel source "__NATION__" "n1")
            (: (|| (== n_name "__Q7_NATION1__") (== n_name "__Q7_NATION2__")) u8))
          ((s_nationkey n_nationkey)) (inner))
        ((l_suppkey s_suppkey)) (inner))
      (: (|| (&& (== |n1.n_name| "__Q7_NATION1__")   (== |n2.n_name| "__Q7_NATION2__"))
             (&& (== |n1.n_name| "__Q7_NATION2__")   (== |n2.n_name| "__Q7_NATION1__"))) u8))
    (supp_nation |n1.n_name|)
    (cust_nation |n2.n_name|)
    (l_year      (call qdb_date_year l_shipdate))
    (volume      (* l_extendedprice (- (: 1.0 f64) l_discount))))
  (keys supp_nation cust_nation l_year)
  (agg revenue sum volume))
