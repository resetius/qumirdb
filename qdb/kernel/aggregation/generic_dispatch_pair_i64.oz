(block
  (pragma language overloads)

  (type PairKey <struct (first i64) (second i64)>)

  (fun rh_hash ((var key <named PairKey>)) -> i64
    (block
      (return (+ (* (field key first) (: 100 i64))
                 (field key second)))))

  (fun rh_hash_generic ((var key <named Key (template)>)) -> i64
    (block
      (return (call rh_hash key))))

  ;; The runner returns the appended rh_hash_generic<PairKey> specialization.
  (fun aggregation_dispatch_pair ((var key <named PairKey>)) -> i64
    (block
      (return (call rh_hash_generic key)))))
