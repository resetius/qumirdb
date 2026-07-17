(block
  (pragma language overloads)

  (type PairKey <struct (first i64) (second i64)>)

  (fun rh_mix_i64 ((var value i64)) -> i64
    (block
      (var h = (cast value u64))
      (= h (^ h (>> h (: 12 u64))))
      (= h (^ h (<< h (: 25 u64))))
      (= h (^ h (>> h (: 27 u64))))
      (= h (* h (: 2685821657736338717 u64)))
      (return (cast h i64))))

  (fun rh_hash ((var key <named PairKey>)) -> i64
    (block
      (return (call rh_mix_i64
        (^ (field key first)
             (call rh_mix_i64 (field key second)))))))

  (fun rh_rehash [Key] ((var old_keys <ptr Key>)
                  (var old_dist <ptr i64>)
                  (var old_slot_ids <ptr i64>)
                  (var old_capacity i64)
                  (var size i64)
                  (var old_group_keys <ptr Key>)
                  (var old_counts <ptr i64>)
                  (var old_sums <ptr i64>)
                  (var new_keys <ptr Key>)
                  (var new_dist <ptr i64>)
                  (var new_slot_ids <ptr i64>)
                  (var new_capacity i64)
                  (var new_group_keys <ptr Key>)
                  (var new_counts <ptr i64>)
                  (var new_sums <ptr i64>)) -> i64
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
              (var slot = (& (call rh_hash carried_key) (- new_capacity (: 1 i64))))
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
                      (= slot (& (+ slot (: 1 i64)) (- new_capacity (: 1 i64))))
                      (= carried_dist (+ carried_dist (: 1 i64)))
                      (= probes (+ probes (: 1 i64)))))))
              (if (! inserted) (block (return (: -1 i64))))))
          (= index (+ index (: 1 i64)))))

      (= index (: 0 i64))
      (while (< index size)
        (block
          (= new_group_keys [index] (index old_group_keys index))
          (= new_counts [index] (index old_counts index))
          (= new_sums [index] (index old_sums index))
          (= index (+ index (: 1 i64)))))
      (return size)))

  ;; The runner returns rh_rehash<PairKey>; this wrapper has the same ABI.
  (fun aggregation_pair_rehash ((var old_keys <ptr <named PairKey>>)
                                (var old_dist <ptr i64>)
                                (var old_slot_ids <ptr i64>)
                                (var old_capacity i64)
                                (var size i64)
                                (var old_group_keys <ptr <named PairKey>>)
                                (var old_counts <ptr i64>)
                                (var old_sums <ptr i64>)
                                (var new_keys <ptr <named PairKey>>)
                                (var new_dist <ptr i64>)
                                (var new_slot_ids <ptr i64>)
                                (var new_capacity i64)
                                (var new_group_keys <ptr <named PairKey>>)
                                (var new_counts <ptr i64>)
                                (var new_sums <ptr i64>)) -> i64
    (block
      (return (call rh_rehash old_keys old_dist old_slot_ids old_capacity size
                    old_group_keys old_counts old_sums new_keys new_dist
                    new_slot_ids new_capacity new_group_keys new_counts new_sums)))))
