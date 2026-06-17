(rel join
  (rel join
    (rel join
      (rel join
        (rel filter (rel source "__PART__") (: (== p_size (: __Q2_PART_SIZE__ i32)) u8))
        (rel source "__PARTSUPP__") ((p_partkey ps_partkey)) (inner))
      (rel source "__SUPPLIER__") ((ps_suppkey s_suppkey)) (inner))
    (rel source "__NATION__") ((s_nationkey n_nationkey)) (inner))
  (rel filter (rel source "__REGION__") (: (== r_name "__Q2_REGION__") u8))
  ((n_regionkey r_regionkey)) (inner))
