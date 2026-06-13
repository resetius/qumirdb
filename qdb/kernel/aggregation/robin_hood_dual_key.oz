(block
  (pragma language overloads)

  (fun rh_lookup_dual
       ((var keys <ptr <named StoredKey (template readable mutable)>>)
        (var dist <ptr i64>)
        (var slot_ids <ptr i64>)
        (var capacity i64)
        (var key <named LookupKey (template readable mutable)>)) -> i64
    (block
      (var slot = (& (call rh_hash key) (- capacity (: 1 i64))))
      (var probe_dist i64)
      (= probe_dist (: 0 i64))
      (while (< probe_dist capacity)
        (block
          (var resident_dist = (index dist slot))
          (if (|| (== resident_dist (: -1 i64)) (< resident_dist probe_dist))
            (block (return (: -1 i64))))
          (if (call rh_key_equal (index keys slot) key)
            (block (return (index slot_ids slot))))
          (= slot (& (+ slot (: 1 i64)) (- capacity (: 1 i64))))
          (= probe_dist (+ probe_dist (: 1 i64)))))
      (return (: -1 i64))))

  (fun rh_insert_stored
       ((var keys <ptr <named StoredKey (template readable mutable)>>)
        (var dist <ptr i64>)
        (var slot_ids <ptr i64>)
        (var capacity i64)
        (var key <named StoredKey (template readable mutable)>)
        (var dense_slot i64)) -> bool
    (block
      (var carried_key = key)
      (var carried_dist i64)
      (= carried_dist (: 0 i64))
      (var carried_slot = dense_slot)
      (var slot = (& (call rh_hash key) (- capacity (: 1 i64))))
      (var probes i64)
      (= probes (: 0 i64))
      (while (< probes capacity)
        (block
          (var resident_dist = (index dist slot))
          (if (== resident_dist (: -1 i64))
            (block
              (= keys [slot] carried_key)
              (= dist [slot] carried_dist)
              (= slot_ids [slot] carried_slot)
              (return #t)))
          (if (< resident_dist carried_dist)
            (block
              (var resident_key = (index keys slot))
              (var resident_slot = (index slot_ids slot))
              (= keys [slot] carried_key)
              (= dist [slot] carried_dist)
              (= slot_ids [slot] carried_slot)
              (= carried_key resident_key)
              (= carried_dist resident_dist)
              (= carried_slot resident_slot)))
          (= slot (& (+ slot (: 1 i64)) (- capacity (: 1 i64))))
          (= carried_dist (+ carried_dist (: 1 i64)))
          (= probes (+ probes (: 1 i64)))))
      (return #f))))
