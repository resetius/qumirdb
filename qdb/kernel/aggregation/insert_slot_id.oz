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

  (fun rh_insert_slot_id ((var keys <ptr <named Key (template)>>)
                          (var dist <ptr i64>)
                          (var slot_ids <ptr i64>)
                          (var capacity i64)
                          (var size <ptr i64>)
                          (var key <named Key (template)>)) -> i64
    (block
      (if (>= (index size (: 0 i64)) capacity)
        (block
          (return (: -1 i64))))
      (var new_slot = (index size (: 0 i64)))
      (var carried = key)
      (var carried_dist i64)
      (= carried_dist (: 0 i64))
      (var carried_slot = new_slot)
      (var index = (& (call rh_hash key) (- capacity (: 1 i64))))
      (var probes i64)
      (= probes (: 0 i64))
      (while (< probes capacity)
        (block
          (var resident_dist = (index dist index))
          (if (== resident_dist (: -1 i64))
            (block
              (= keys [index] carried)
              (= dist [index] carried_dist)
              (= slot_ids [index] carried_slot)
              (= size [(: 0 i64)] (+ new_slot (: 1 i64)))
              (return new_slot)))
          (if (call rh_key_equal (index keys index) key)
            (block
              (return (index slot_ids index))))
          (if (< resident_dist carried_dist)
            (block
              (var resident = (index keys index))
              (var resident_slot = (index slot_ids index))
              (= keys [index] carried)
              (= dist [index] carried_dist)
              (= slot_ids [index] carried_slot)
              (= carried resident)
              (= carried_dist resident_dist)
              (= carried_slot resident_slot)))
          (= index (& (+ index (: 1 i64)) (- capacity (: 1 i64))))
          (= carried_dist (+ carried_dist (: 1 i64)))
          (= probes (+ probes (: 1 i64)))))
      (return (: -1 i64))))

  (fun aggregation_insert_slot_id ((var keys <ptr i64>)
                                   (var dist <ptr i64>)
                                   (var slot_ids <ptr i64>)
                                   (var capacity i64)
                                   (var size <ptr i64>)
                                   (var key i64)) -> i64
    (block
      (return (call rh_insert_slot_id keys dist slot_ids capacity size key)))))
