(rel join
  (rel join
    (rel join
      (rel join
        (rel source "__PARTSUPP__")
        (rel filter (rel source "__PART__") (: (== p_size (: __Q2_PART_SIZE__ i32)) u8))
        ((ps_partkey p_partkey)) (inner))
      (rel source "__SUPPLIER__") ((ps_suppkey s_suppkey)) (inner))
    (rel source "__NATION__") ((s_nationkey n_nationkey)) (inner))
  (rel filter (rel source "__REGION__") (: (== r_name "__Q2_REGION__") u8))
  ((n_regionkey r_regionkey)) (inner))
