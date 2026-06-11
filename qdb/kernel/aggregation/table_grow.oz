(block
  (fun rh_hash_i64 ((var key i64)) -> i64
    (block
      (var h = (cast key u64))
      (= h (xor h (>> h (: 12 u64))))
      (= h (xor h (<< h (: 25 u64))))
      (= h (xor h (>> h (: 27 u64))))
      (= h (* h (: 2685821657736338717 u64)))
      (return (cast h i64))))

  (fun rh_lookup_table ((var ht <ref HashTable>) (var key i64)) -> i64
    (block
      (var capacity = (field ht Capacity))
      (if (<= capacity (: 0 i64))
        (block (return (: -1 i64))))
      (var keys = (field ht Keys))
      (var dist = (field ht Dist))
      (var slot_ids = (field ht SlotId))
      (var slot = (& (call rh_hash_i64 key) (- capacity (: 1 i64))))
      (var probe_dist i64)
      (= probe_dist (: 0 i64))
      (while (< probe_dist capacity)
        (block
          (var resident_dist = (index dist slot))
          (if (|| (== resident_dist (: -1 i64)) (< resident_dist probe_dist))
            (block (return (: -1 i64))))
          (if (== (index keys slot) key)
            (block (return (index slot_ids slot))))
          (= slot (& (+ slot (: 1 i64)) (- capacity (: 1 i64))))
          (= probe_dist (+ probe_dist (: 1 i64)))))
      (return (: -1 i64))))

  ;; Inserts an already assigned dense slot id while rehashing.
  (fun rh_insert_existing ((var keys <ptr i64>)
                           (var dist <ptr i64>)
                           (var slot_ids <ptr i64>)
                           (var capacity i64)
                           (var key i64)
                           (var dense_slot i64)) -> bool
    (block
      (var carried_key = key)
      (var carried_dist i64)
      (= carried_dist (: 0 i64))
      (var carried_slot = dense_slot)
      (var slot = (& (call rh_hash_i64 key) (- capacity (: 1 i64))))
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
      (return #f)))

  (fun rh_rehash ((var ht <ref HashTable>) (var new_capacity i64)) -> bool
    (block
      (var bytes = (* new_capacity (: 8 i64)))
      (var new_keys = (cast (call qdb_alloc bytes) <ptr i64>))
      (var new_dist = (cast (call qdb_alloc bytes) <ptr i64>))
      (var new_slot_ids = (cast (call qdb_alloc bytes) <ptr i64>))
      (if (|| (== (cast new_keys i64) (: 0 i64))
              (|| (== (cast new_dist i64) (: 0 i64))
                  (== (cast new_slot_ids i64) (: 0 i64))))
        (block
          (if (!= (cast new_keys i64) (: 0 i64))
            (block (call qdb_free (cast new_keys <ptr i8>))))
          (if (!= (cast new_dist i64) (: 0 i64))
            (block (call qdb_free (cast new_dist <ptr i8>))))
          (if (!= (cast new_slot_ids i64) (: 0 i64))
            (block (call qdb_free (cast new_slot_ids <ptr i8>))))
          (return #f)))
      (var i i64)
      (= i (: 0 i64))
      (while (< i new_capacity)
        (block
          (= new_keys [i] (: 0 i64))
          (= new_dist [i] (: -1 i64))
          (= new_slot_ids [i] (: -1 i64))
          (= i (+ i (: 1 i64)))))

      (var old_keys = (field ht Keys))
      (var old_dist = (field ht Dist))
      (var old_slot_ids = (field ht SlotId))
      (var old_capacity = (field ht Capacity))
      (= i (: 0 i64))
      (while (< i old_capacity)
        (block
          (if (!= (index old_dist i) (: -1 i64))
            (block
              (if (! (call rh_insert_existing new_keys new_dist new_slot_ids
                      new_capacity (index old_keys i) (index old_slot_ids i)))
                (block
                  (call qdb_free (cast new_keys <ptr i8>))
                  (call qdb_free (cast new_dist <ptr i8>))
                  (call qdb_free (cast new_slot_ids <ptr i8>))
                  (return #f)))))
          (= i (+ i (: 1 i64)))))

      (field_assign ht Keys new_keys)
      (field_assign ht Dist new_dist)
      (field_assign ht SlotId new_slot_ids)
      (field_assign ht Capacity new_capacity)
      (call qdb_free (cast old_keys <ptr i8>))
      (call qdb_free (cast old_dist <ptr i8>))
      (call qdb_free (cast old_slot_ids <ptr i8>))
      (return #t)))

  (fun rh_insert_table ((var ht <ref HashTable>) (var key i64)) -> i64
    (block
      (var existing = (call rh_lookup_table ht key))
      (if (!= existing (: -1 i64))
        (block (return existing)))
      (var capacity = (field ht Capacity))
      (var size = (field ht Size))
      (if (> (* (+ size (: 1 i64)) (: 4 i64)) (* capacity (: 3 i64)))
        (block
          (if (! (call rh_rehash ht (* capacity (: 2 i64))))
            (block (return (: -1 i64))))
          (= capacity (field ht Capacity))))

      (var keys = (field ht Keys))
      (var dist = (field ht Dist))
      (var slot_ids = (field ht SlotId))
      (var carried_key = key)
      (var carried_dist i64)
      (= carried_dist (: 0 i64))
      (var carried_slot = size)
      (var slot = (& (call rh_hash_i64 key) (- capacity (: 1 i64))))
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
              (field_assign ht Size (+ size (: 1 i64)))
              (return size)))
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
      (return (: -1 i64))))

  ;; op=0 inserts, op!=0 performs a read-only lookup. This function is last.
  (fun aggregation_table_grow ((var ht <ref HashTable>)
                               (var key i64)
                               (var op i64)) -> i64
    (block
      (return
        (if (== op (: 0 i64))
          (call rh_insert_table ht key)
          (call rh_lookup_table ht key))))))
