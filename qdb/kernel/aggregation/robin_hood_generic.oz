(block
  (pragma language overloads)

  ;; Returns the stable dense slot id. out_is_new[0] is 1 for a new key and
  ;; 0 for an existing key. Key representation is visible only through the
  ;; injected rh_hash/rh_key_equal overloads.
  (fun rh_upsert ((var keys <ptr <named Key (template)>>)
                  (var dist <ptr i64>)
                  (var slot_ids <ptr i64>)
                  (var capacity i64)
                  (var size <ptr i64>)
                  (var key <named Key (template)>)
                  (var out_is_new <ptr i64>)) -> i64
    (block
      (= out_is_new [(: 0 i64)] (: 0 i64))
      (var slot = (& (call rh_hash key) (- capacity (: 1 i64))))
      (var probe_dist i64)
      (= probe_dist (: 0 i64))
      (var searching bool)
      (= searching #t)
      (while (&& searching (< probe_dist capacity))
        (block
          (var resident_dist = (index dist slot))
          (if (|| (== resident_dist (: -1 i64)) (< resident_dist probe_dist))
            (block (= searching #f))
            (block
              (if (call rh_key_equal (index keys slot) key)
                (block (return (index slot_ids slot))))))
          (if searching
            (block
              (= slot (& (+ slot (: 1 i64)) (- capacity (: 1 i64))))
              (= probe_dist (+ probe_dist (: 1 i64)))))))

      (var dense_slot = (index size (: 0 i64)))
      (if (>= dense_slot capacity) (block (return (: -1 i64))))
      (var carried_key = key)
      (var carried_dist i64)
      (= carried_dist (: 0 i64))
      (var carried_slot = dense_slot)
      (= slot (& (call rh_hash key) (- capacity (: 1 i64))))
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
              (= size [(: 0 i64)] (+ dense_slot (: 1 i64)))
              (= out_is_new [(: 0 i64)] (: 1 i64))
              (return dense_slot)))
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
      (return (: -1 i64)))))
