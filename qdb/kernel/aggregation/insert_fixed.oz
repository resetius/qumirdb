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

  (fun rh_insert_fixed [Key] ((var keys <ptr Key>)
                        (var dist <ptr i64>)
                        (var capacity i64)
                        (var key Key)) -> i64
    (block
      (var carried = key)
      (var carried_dist i64)
      (= carried_dist (: 0 i64))
      (var index = (call rh_home (call rh_hash key) capacity))
      (var query_index i64)
      (= query_index (: -1 i64))
      (var probes i64)
      (= probes (: 0 i64))
      (while (< probes capacity)
        (block
          (var resident_dist = (index dist index))
          (if (== resident_dist (: -1 i64))
            (block
              (= keys [index] carried)
              (= dist [index] carried_dist)
              (return (if (== query_index (: -1 i64)) index query_index))))
          (if (call rh_key_equal (index keys index) key)
            (block
              (return index)))
          (if (< resident_dist carried_dist)
            (block
              (var resident = (index keys index))
              (= keys [index] carried)
              (= dist [index] carried_dist)
              (if (call rh_key_equal carried key)
                (block
                  (= query_index index)))
              (= carried resident)
              (= carried_dist resident_dist)))
          (= index (call rh_next_index index capacity))
          (= carried_dist (+ carried_dist (: 1 i64)))
          (= probes (+ probes (: 1 i64)))))
      (return (: -1 i64))))

  ;; Instantiates rh_insert_fixed<Key=i64>; its specialization is returned by
  ;; CompileKernel and has this same ABI.
  (fun aggregation_insert_fixed ((var keys <ptr i64>)
                                 (var dist <ptr i64>)
                                 (var capacity i64)
                                 (var key i64)) -> i64
    (block
      (return (call rh_insert_fixed keys dist capacity key)))))
