(rel sort
  (rel project
    (rel join
      (rel join
        (rel source "__SUPPLIER__")
        (rel filter (rel source "__NATION__") (: (== n_name "__Q20_NATION__") u8))
        ((s_nationkey n_nationkey)) (inner))
      (rel project
        (rel filter
          (rel join
            (rel join
              (rel source "__PARTSUPP__")
              (rel filter (rel source "__PART__")
                (: (call qdb_string_view_sql_like p_name "thistle%") u8))
              ((ps_partkey p_partkey)) (left_semi))
            (rel project
              (rel aggregate
                (rel filter
                  (rel source "__LINEITEM__")
                  (: (&& (>= l_shipdate (: __Q20_DATE_LOW__ i32))
                         (< l_shipdate (: __Q20_DATE_HIGH__ i32))) u8))
                (keys l_partkey l_suppkey)
                (agg sum_qty sum l_quantity))
              (thr_partkey l_partkey)
              (thr_suppkey l_suppkey)
              (threshold   (* sum_qty (: 0.5 f64))))
            ((ps_partkey thr_partkey) (ps_suppkey thr_suppkey)) (inner))
          (: (> ps_availqty threshold) u8))
        (qualifying_suppkey ps_suppkey))
      ((s_suppkey qualifying_suppkey)) (left_semi))
    (s_name    s_name)
    (s_address s_address))
  (s_name asc nulls-default))
