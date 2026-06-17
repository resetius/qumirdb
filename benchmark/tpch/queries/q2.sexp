(rel project
  (rel filter
    (rel join
      (rel join
        (rel join
          (rel join
            (rel source "__PARTSUPP__")
            (rel filter (rel source "__PART__")
              (: (&& (== p_size (: __Q2_PART_SIZE__ i32))
                     (call qdb_string_view_sql_like p_type "%BRASS")) u8))
            ((ps_partkey p_partkey)) (inner))
          (rel source "__SUPPLIER__") ((ps_suppkey s_suppkey)) (inner))
        (rel source "__NATION__") ((s_nationkey n_nationkey)) (inner))
      (rel filter (rel source "__REGION__") (: (== r_name "__Q2_REGION__") u8))
      ((n_regionkey r_regionkey)) (inner))
    (: (== ps_supplycost (scalar_subquery_min_supplycost p_partkey)) u8))
  (s_acctbal s_acctbal)
  (s_name    s_name)
  (n_name    n_name)
  (p_partkey p_partkey)
  (p_mfgr    p_mfgr)
  (s_address s_address)
  (s_phone   s_phone)
  (s_comment s_comment))
