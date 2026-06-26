(block
  (pragma language overloads)

  ;; Key-specific operations are overloads. Stage 1 intentionally provides
  ;; only i64; later key types can be added without rewriting the algorithm.
  (fun rh_hash ((var key i64)) -> i64
    (block
      (return key)))

  ;; Robin Hood helpers use a template Key and depend only on the key
  ;; operation contract above.
  (fun rh_hash_key ((var key <named Key (template)>)) -> i64
    (block
      (return (call rh_hash key))))

  (fun aggregation_generic_key_smoke ((var key i64)) -> i64
    (block
      (return (call rh_hash_key key)))))
