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

  (fun rh_key_equal ((var left <named PairKey>)
                     (var right <named PairKey>)) -> bool
    (block
      (return (&& (== (field left first) (field right first))
                  (== (field left second) (field right second))))))

  (fun rh_process_batch [Key] ((var keys <ptr Key>)
                         (var dist <ptr i64>)
                         (var slot_ids <ptr i64>)
                         (var capacity i64)
                         (var size <ptr i64>)
                         (var group_keys <ptr Key>)
                         (var counts <ptr i64>)
                         (var sums <ptr i64>)
                         (var input_keys <ptr Key>)
                         (var input_values <ptr i64>)
                         (var length i64)) -> i64
    (block
      (var row i64)
      (= row (: 0 i64))
      (while (< row length)
        (block
          (var old_size = (index size (: 0 i64)))
          (var key = (index input_keys row))
          (var dense_slot i64)
          (= dense_slot (: -1 i64))

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
                    (block
                      (= dense_slot (index slot_ids slot))
                      (= searching #f)))))
              (if searching
                (block
                  (= slot (& (+ slot (: 1 i64)) (- capacity (: 1 i64))))
                  (= probe_dist (+ probe_dist (: 1 i64)))))))

          (if (== dense_slot (: -1 i64))
            (block
              (if (>= old_size capacity) (block (return (: -1 i64))))
              (= dense_slot old_size)
              (var carried_key = key)
              (var carried_dist i64)
              (= carried_dist (: 0 i64))
              (var carried_slot = dense_slot)
              (= slot (& (call rh_hash key) (- capacity (: 1 i64))))
              (var probes i64)
              (= probes (: 0 i64))
              (var inserted bool)
              (= inserted #f)
              (while (&& (! inserted) (< probes capacity))
                (block
                  (var resident_dist = (index dist slot))
                  (if (== resident_dist (: -1 i64))
                    (block
                      (= keys [slot] carried_key)
                      (= dist [slot] carried_dist)
                      (= slot_ids [slot] carried_slot)
                      (= size [(: 0 i64)] (+ old_size (: 1 i64)))
                      (= inserted #t))
                    (block
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
                      (= probes (+ probes (: 1 i64)))))))
              (if (! inserted) (block (return (: -1 i64))))))
          (if (== dense_slot old_size)
            (block
              (= group_keys [dense_slot] key)
              (= counts [dense_slot] (: 0 i64))
              (= sums [dense_slot] (: 0 i64))))
          (= counts [dense_slot] (+ (index counts dense_slot) (: 1 i64)))
          (= sums [dense_slot] (+ (index sums dense_slot) (index input_values row)))
          (= row (+ row (: 1 i64)))))
      (return (index size (: 0 i64)))))

  ;; The runner returns rh_process_batch<PairKey>; this wrapper has the same ABI.
  (fun aggregation_pair_fixed ((var keys <ptr <named PairKey>>)
                               (var dist <ptr i64>)
                               (var slot_ids <ptr i64>)
                               (var capacity i64)
                               (var size <ptr i64>)
                               (var group_keys <ptr <named PairKey>>)
                               (var counts <ptr i64>)
                               (var sums <ptr i64>)
                               (var input_keys <ptr <named PairKey>>)
                               (var input_values <ptr i64>)
                               (var length i64)) -> i64
    (block
      (return (call rh_process_batch keys dist slot_ids capacity size group_keys
                    counts sums input_keys input_values length)))))
