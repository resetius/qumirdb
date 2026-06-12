(block
  (pragma language overloads)

  (fun rh_hash ((var key i64)) -> i64
    (block
      (var h = (cast key u64))
      (= h (xor h (>> h (: 12 u64))))
      (= h (xor h (<< h (: 25 u64))))
      (= h (xor h (>> h (: 27 u64))))
      (= h (* h (: 2685821657736338717 u64)))
      (return (cast h i64))))

  (fun rh_key_equal ((var left i64) (var right i64)) -> bool
    (block
      (return (== left right)))))
