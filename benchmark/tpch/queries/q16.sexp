(rel aggregate
  (rel aggregate
    (rel join
      (rel join
        (rel filter (rel source "__PART__")
          (: (&& (!= p_brand "__Q16_BRAND__")
                 (&& (== (call qdb_string_view_sql_like p_type "PROMO ANODIZED%") (: 0 u8))
                     (|| (== p_size (: 49 i32)) (|| (== p_size (: 14 i32)) (|| (== p_size (: 23 i32))
                     (|| (== p_size (: 45 i32)) (|| (== p_size (: 19 i32)) (|| (== p_size (: 3 i32))
                     (|| (== p_size (: 36 i32))     (== p_size (: 9 i32))))))))))) u8))
        (rel source "__PARTSUPP__") ((p_partkey ps_partkey)) (inner))
      (rel filter (rel source "__SUPPLIER__")
        (: (call qdb_string_view_sql_like s_comment "%Customer%Complaints%") u8))
      ((ps_suppkey s_suppkey)) (left_anti))
    (keys p_brand p_type p_size ps_suppkey))
  (keys p_brand p_type p_size)
  (agg supplier_cnt count ps_suppkey))
