(rel aggregate
  (rel join
    (rel filter (rel source "__PART__")
      (: (&& (!= p_brand "__Q16_BRAND__") (> p_size (: 0 i32))) u8))
    (rel source "__PARTSUPP__") ((p_partkey ps_partkey)) (inner))
  (keys p_brand p_type p_size)
  (agg supplier_cnt count))
