(rel aggregate
  (rel project
    (rel filter (rel source "__CUSTOMER__")
      (: (> c_acctbal (: 0.0 f64)) u8))
    (c_phone c_phone)
    (c_acctbal c_acctbal)
    (dummy (: 1 i64)))
  (keys c_phone)
  (agg numcust count)
  (agg totacctbal sum c_acctbal))
