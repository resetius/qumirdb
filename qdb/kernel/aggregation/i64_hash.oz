(block
  ;; Stage 1 key operation: xorshift64* over the complete i64 bit pattern.
  (fun rh_hash ((var key i64)) -> i64
    (block
      (var h = (cast key u64))
      (= h (^ h (>> h (: 12 u64))))
      (= h (^ h (<< h (: 25 u64))))
      (= h (^ h (>> h (: 27 u64))))
      (= h (* h (: 2685821657736338717 u64)))
      (return (cast h i64))))

  (fun aggregation_i64_hash ((var key i64)) -> i64
    (block
      (return (call rh_hash key)))))
