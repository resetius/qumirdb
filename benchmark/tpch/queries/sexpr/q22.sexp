(rel sort
  (rel aggregate
    (rel project
      (rel join
        (rel filter
          (rel join
            (rel filter (rel source "__CUSTOMER__")
              (: (|| (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC1__")
                 (|| (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC2__")
                 (|| (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC3__")
                 (|| (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC4__")
                 (|| (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC5__")
                 (|| (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC6__")
                     (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC7__"))))))) u8))
            (rel project
              (rel aggregate
                (rel project
                  (rel filter (rel source "__CUSTOMER__")
                    (: (&& (> c_acctbal (: 0.0 f64))
                           (|| (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC1__")
                           (|| (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC2__")
                           (|| (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC3__")
                           (|| (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC4__")
                           (|| (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC5__")
                           (|| (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC6__")
                               (== (call qdb_substring c_phone (: 1 i32) (: 2 i32)) "__Q22_CC7__")))))))) u8))
                  (c_acctbal c_acctbal)
                  (dummy (: 1 i64)))
                (keys dummy)
                (agg sum_acctbal sum c_acctbal)
                (agg cnt_acctbal count c_acctbal))
              (avg_acctbal (/ sum_acctbal (cast cnt_acctbal f64))))
            () (inner))
          (: (> c_acctbal avg_acctbal) u8))
        (rel source "__ORDERS__") ((c_custkey o_custkey)) (left_anti))
      (cntrycode (call qdb_substring c_phone (: 1 i32) (: 2 i32)))
      (c_acctbal c_acctbal))
    (keys cntrycode)
    (agg numcust count)
    (agg totacctbal sum c_acctbal))
  (cntrycode asc nulls-default))
