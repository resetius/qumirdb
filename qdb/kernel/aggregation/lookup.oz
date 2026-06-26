(block
  (pragma language overloads)

  (fun rh_hash ((var key i64)) -> i64
    (block
      (var h = (cast key u64))
      (= h (^ h (>> h (: 12 u64))))
      (= h (^ h (<< h (: 25 u64))))
      (= h (^ h (>> h (: 27 u64))))
      (= h (* h (: 2685821657736338717 u64)))
      (return (cast h i64))))

  (fun rh_key_equal ((var left i64) (var right i64)) -> bool
    (block
      (return (== left right))))

  (fun rh_home ((var hash i64) (var capacity i64)) -> i64
    (block
      (return (& hash (- capacity (: 1 i64))))))

  (fun rh_next_index ((var index i64) (var capacity i64)) -> i64
    (block
      (return (& (+ index (: 1 i64)) (- capacity (: 1 i64))))))

  (fun rh_find_slot ((var keys <ptr <named Key (template)>>)
                     (var dist <ptr i64>)
                     (var capacity i64)
                     (var query <named Key (template)>)) -> i64
    (block
      (var index = (call rh_home (call rh_hash query) capacity))
      (var query_dist i64)
      (= query_dist (: 0 i64))
      (var probes i64)
      (= probes (: 0 i64))
      (while (< probes capacity)
        (block
          (var resident_dist = (index dist index))
          (if (== resident_dist (: -1 i64))
            (block
              (return (: -1 i64))))
          (if (< resident_dist query_dist)
            (block
              (return (: -1 i64))))
          (if (call rh_key_equal (index keys index) query)
            (block
              (return index)))
          (= index (call rh_next_index index capacity))
          (= query_dist (+ query_dist (: 1 i64)))
          (= probes (+ probes (: 1 i64)))))
      (return (: -1 i64))))

  ;; Instantiates rh_find_slot<Key=i64>. CompileKernel currently returns the
  ;; generated specialization, whose ABI is identical to this wrapper.
  (fun aggregation_lookup ((var keys <ptr i64>)
                           (var dist <ptr i64>)
                           (var capacity i64)
                           (var query i64)) -> i64
    (block
      (return (call rh_find_slot keys dist capacity query)))))
