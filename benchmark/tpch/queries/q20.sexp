(rel project
  (rel join
    (rel join
      (rel source "__SUPPLIER__")
      (rel filter (rel source "__NATION__") (: (== n_name "__Q20_NATION__") u8))
      ((s_nationkey n_nationkey)) (inner))
    (rel filter
      (rel join
        (rel source "__PARTSUPP__")
        (rel filter (rel source "__PART__")
          (: (qdb_string_view_sql_like p_name "thistle%") u8))
        ((ps_partkey p_partkey)) (left_semi))
      (: (> ps_availqty (scalar_subquery_lineitem_qty ps_partkey ps_suppkey)) u8))
    ((s_suppkey ps_suppkey)) (left_semi))
  (s_name s_name)
  (s_address s_address))
