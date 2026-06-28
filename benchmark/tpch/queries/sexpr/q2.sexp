(rel top-sort
  (rel project
    (rel filter
      (rel join
        (rel join
          (rel join
            (rel source "__PARTSUPP__")
            (rel filter (rel source "__PART__")
              (: (&& (== p_size (: __Q2_PART_SIZE__ i32))
                     (call qdb_string_view_sql_like p_type "%BRASS")) u8))
            ((ps_partkey p_partkey)) (inner))
          (rel join
            (rel source "__SUPPLIER__")
            (rel join
              (rel source "__NATION__")
              (rel filter (rel source "__REGION__")
                (: (== r_name "__Q2_REGION__") u8))
              ((n_regionkey r_regionkey)) (inner))
            ((s_nationkey n_nationkey)) (inner))
          ((ps_suppkey s_suppkey)) (inner))
        (rel project
          (rel aggregate
            (rel join
              (rel source "__PARTSUPP__")
              (rel join
                (rel source "__SUPPLIER__")
                (rel join
                  (rel source "__NATION__")
                  (rel filter (rel source "__REGION__")
                    (: (== r_name "__Q2_REGION__") u8))
                  ((n_regionkey r_regionkey)) (left_semi))
                ((s_nationkey n_nationkey)) (left_semi))
              ((ps_suppkey s_suppkey)) (left_semi))
            (keys ps_partkey)
            (agg min_supplycost min ps_supplycost))
          (min_partkey    ps_partkey)
          (min_supplycost min_supplycost))
        ((ps_partkey min_partkey)) (inner))
      (: (== ps_supplycost min_supplycost) u8))
    (s_acctbal s_acctbal)
    (s_name    s_name)
    (n_name    n_name)
    (p_partkey p_partkey)
    (p_mfgr    p_mfgr)
    (s_address s_address)
    (s_phone   s_phone)
    (s_comment s_comment))
  (s_acctbal desc nulls-default) (n_name asc nulls-default) (s_name asc nulls-default) (p_partkey asc nulls-default) (limit 100))
