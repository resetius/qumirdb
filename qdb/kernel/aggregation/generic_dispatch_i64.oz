(block
  (pragma language overloads)

  (fun rh_hash ((var key i64)) -> i64
    (block
      (return (+ key (: 1000 i64)))))

  (fun rh_hash_generic [Key] ((var key Key)) -> i64
    (block
      (return (call rh_hash key))))

  ;; The runner returns the appended rh_hash_generic<i64> specialization.
  (fun aggregation_dispatch_i64 ((var key i64)) -> i64
    (block
      (return (call rh_hash_generic key)))))
