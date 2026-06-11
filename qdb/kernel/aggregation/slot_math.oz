(block
  ;; Capacity is a power of two. Masking keeps the home slot non-negative even
  ;; when the hash has its sign bit set.
  (fun rh_home ((var hash i64) (var capacity i64)) -> i64
    (block
      (return (& hash (- capacity (: 1 i64))))))

  (fun rh_next_index ((var index i64) (var capacity i64)) -> i64
    (block
      (return (& (+ index (: 1 i64)) (- capacity (: 1 i64))))))

  ;; op=0 returns home(hash, capacity); any other value advances an index.
  (fun aggregation_slot_math ((var value i64)
                              (var capacity i64)
                              (var op i64)) -> i64
    (block
      (return (if (== op (: 0 i64))
                  (call rh_home value capacity)
                  (call rh_next_index value capacity))))))
