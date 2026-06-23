(rel aggregate
  (rel aggregate
    (rel join
      (rel source "__CUSTOMER__")
      (rel filter (rel source "__ORDERS__")
        (: (== (call qdb_string_view_sql_like o_comment "%special%packages%") (: 0 u8)) u8))
      ((c_custkey o_custkey)) (left))
    (keys c_custkey)
    (agg c_count count))
  (keys c_count)
  (agg custdist count))
