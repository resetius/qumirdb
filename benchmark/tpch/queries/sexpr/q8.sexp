(rel sort
  (rel project
    (rel aggregate
      (rel project
        (rel join
          (rel join
            (rel join
              (rel join
                (rel join
                  (rel join
                    (rel join
                      (rel filter (rel source "__PART__") (: (== p_type "__Q8_PTYPE__") u8))
                      (rel source "__LINEITEM__") ((p_partkey l_partkey)) (inner))
                    (rel source "__SUPPLIER__") ((l_suppkey s_suppkey)) (inner))
                  (rel filter (rel source "__ORDERS__")
                    (: (&& (>= o_orderdate (: __Q8_DATE_LOW__ i32))
                           (<= o_orderdate (: __Q8_DATE_HIGH__ i32))) u8))
                  ((l_orderkey o_orderkey)) (inner))
                (rel source "__CUSTOMER__") ((o_custkey c_custkey)) (inner))
              (rel source "__NATION__" "n1") ((c_nationkey n_nationkey)) (inner))
            (rel filter (rel source "__REGION__") (: (== r_name "__Q8_REGION__") u8))
            ((n_regionkey r_regionkey)) (inner))
          (rel source "__NATION__" "n2") ((s_nationkey n_nationkey)) (inner))
        (o_year        (call qdb_date_year o_orderdate))
        (volume        (* l_extendedprice (- (: 1.0 f64) l_discount)))
        (france_volume (if (== |n2.n_name| "__Q8_NATION__")
                         (* l_extendedprice (- (: 1.0 f64) l_discount))
                         (: 0.0 f64))))
      (keys o_year)
      (agg sum_france sum france_volume)
      (agg sum_total  sum volume))
    (o_year    o_year)
    (mkt_share (/ sum_france sum_total)))
  (o_year asc nulls-default))
