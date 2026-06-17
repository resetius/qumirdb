(rel aggregate
  (rel project
    (rel join
      (rel join
        (rel join
          (rel join
            (rel join
              (rel filter (rel source "__PART__") (: (call qdb_string_view_sql_like p_name "%turquoise%") u8))
              (rel source "__PARTSUPP__") ((p_partkey ps_partkey)) (inner))
            (rel source "__LINEITEM__") ((ps_partkey l_partkey) (ps_suppkey l_suppkey)) (inner))
          (rel source "__SUPPLIER__") ((l_suppkey s_suppkey)) (inner))
        (rel source "__ORDERS__") ((l_orderkey o_orderkey)) (inner))
      (rel source "__NATION__") ((s_nationkey n_nationkey)) (inner))
    (amount (- (* l_extendedprice (- (: 1.0 f64) l_discount)) (* ps_supplycost l_quantity)))
    (n_name n_name)
    (o_year (qdb_date_year o_orderdate)))
  (keys n_name o_year)
  (agg sum_profit sum amount))
