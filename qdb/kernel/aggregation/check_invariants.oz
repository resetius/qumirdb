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
      (return (== left right))))

  (fun rh_check_invariants ((var keys <ptr <named Key (template readable mutable)>>)
                            (var dist <ptr i64>)
                            (var capacity i64)) -> bool
    (block
      (var slot i64)
      (= slot (: 0 i64))
      (while (< slot capacity)
        (block
          (var resident_dist = (index dist slot))
          (if (!= resident_dist (: -1 i64))
            (block
              (if (|| (< resident_dist (: 0 i64)) (>= resident_dist capacity))
                (block
                  (return #f)))
              (var home = (& (call rh_hash (index keys slot))
                             (- capacity (: 1 i64))))
              (var expected = (& (- slot home) (- capacity (: 1 i64))))
              (if (!= resident_dist expected)
                (block
                  (return #f)))
              (var probe = home)
              (var steps i64)
              (= steps (: 0 i64))
              (var found bool)
              (= found #f)
              (while (&& (< steps capacity) (! found))
                (block
                  (if (== (index dist probe) (: -1 i64))
                    (block
                      (return #f)))
                  (if (call rh_key_equal (index keys probe) (index keys slot))
                    (block
                      (= found #t)))
                  (= probe (& (+ probe (: 1 i64)) (- capacity (: 1 i64))))
                  (= steps (+ steps (: 1 i64)))))
              (if (! found)
                (block
                  (return #f)))))
          (= slot (+ slot (: 1 i64)))))
      (return #t)))

  ;; Instantiates rh_check_invariants<Key=i64>; the specialization has the
  ;; same ABI and is returned by CompileKernel.
  (fun aggregation_check_invariants ((var keys <ptr i64>)
                                     (var dist <ptr i64>)
                                     (var capacity i64)) -> bool
    (block
      (return (call rh_check_invariants keys dist capacity)))))
