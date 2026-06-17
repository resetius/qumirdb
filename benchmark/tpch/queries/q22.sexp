(rel aggregate
  (rel project
    (rel join
      (rel filter (rel source "__CUSTOMER__")
        (: (&& (> c_acctbal (scalar_subquery_avg_acctbal))
               (|| (== (qdb_substring c_phone 1 2) "__Q22_CC1__")
               (|| (== (qdb_substring c_phone 1 2) "__Q22_CC2__")
               (|| (== (qdb_substring c_phone 1 2) "__Q22_CC3__")
               (|| (== (qdb_substring c_phone 1 2) "__Q22_CC4__")
               (|| (== (qdb_substring c_phone 1 2) "__Q22_CC5__")
               (|| (== (qdb_substring c_phone 1 2) "__Q22_CC6__")
                   (== (qdb_substring c_phone 1 2) "__Q22_CC7__")))))))) u8))
      (rel source "__ORDERS__") ((c_custkey o_custkey)) (left_anti))
    (cntrycode (qdb_substring c_phone 1 2))
    (c_acctbal c_acctbal))
  (keys cntrycode)
  (agg numcust count)
  (agg totacctbal sum c_acctbal))
