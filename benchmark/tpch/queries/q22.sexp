(rel aggregate
  (rel project
    (rel join
      (rel filter (rel source "__CUSTOMER__")
        (: (&& (> c_acctbal (scalar_subquery_avg_acctbal))
               (|| (== (call qdb_substring c_phone 1 2) "__Q22_CC1__")
               (|| (== (call qdb_substring c_phone 1 2) "__Q22_CC2__")
               (|| (== (call qdb_substring c_phone 1 2) "__Q22_CC3__")
               (|| (== (call qdb_substring c_phone 1 2) "__Q22_CC4__")
               (|| (== (call qdb_substring c_phone 1 2) "__Q22_CC5__")
               (|| (== (call qdb_substring c_phone 1 2) "__Q22_CC6__")
                   (== (call qdb_substring c_phone 1 2) "__Q22_CC7__")))))))) u8))
      (rel source "__ORDERS__") ((c_custkey o_custkey)) (left_anti))
    (cntrycode (call qdb_substring c_phone 1 2))
    (c_acctbal c_acctbal))
  (keys cntrycode)
  (agg numcust count)
  (agg totacctbal sum c_acctbal))
