(block
  (pragma language overloads)

  ;; Rebuilds only the probe table. Dense group-key and aggregate buffers are
  ;; indexed by slot_id and therefore do not move here.
  (fun rh_rehash_into [Key] ((var old_keys <ptr Key>)
                       (var old_dist <ptr i64>)
                       (var old_slot_ids <ptr i64>)
                       (var old_capacity i64)
                       (var new_keys <ptr Key>)
                       (var new_dist <ptr i64>)
                       (var new_slot_ids <ptr i64>)
                       (var new_capacity i64)) -> bool
    (block
      (var index i64)
      (= index (: 0 i64))
      (while (< index new_capacity)
        (block
          (= new_dist [index] (: -1 i64))
          (= new_slot_ids [index] (: -1 i64))
          (= index (+ index (: 1 i64)))))

      (= index (: 0 i64))
      (while (< index old_capacity)
        (block
          (if (!= (index old_dist index) (: -1 i64))
            (block
              (var carried_key = (index old_keys index))
              (var carried_slot = (index old_slot_ids index))
              (var carried_dist i64)
              (= carried_dist (: 0 i64))
              (var slot = (& (call rh_hash carried_key)
                             (- new_capacity (: 1 i64))))
              (var probes i64)
              (= probes (: 0 i64))
              (var inserted bool)
              (= inserted #f)
              (while (&& (! inserted) (< probes new_capacity))
                (block
                  (var resident_dist = (index new_dist slot))
                  (if (== resident_dist (: -1 i64))
                    (block
                      (= new_keys [slot] carried_key)
                      (= new_dist [slot] carried_dist)
                      (= new_slot_ids [slot] carried_slot)
                      (= inserted #t))
                    (block
                      (if (< resident_dist carried_dist)
                        (block
                          (var resident_key = (index new_keys slot))
                          (var resident_slot = (index new_slot_ids slot))
                          (= new_keys [slot] carried_key)
                          (= new_dist [slot] carried_dist)
                          (= new_slot_ids [slot] carried_slot)
                          (= carried_key resident_key)
                          (= carried_dist resident_dist)
                          (= carried_slot resident_slot)))
                      (= slot (& (+ slot (: 1 i64))
                                 (- new_capacity (: 1 i64))))
                      (= carried_dist (+ carried_dist (: 1 i64)))
                      (= probes (+ probes (: 1 i64)))))))
              (if (! inserted) (block (return #f)))))
          (= index (+ index (: 1 i64)))))
      (return #t))))
