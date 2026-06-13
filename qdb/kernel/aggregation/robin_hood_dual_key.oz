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
      (return #f)))

  (fun aht_upsert_dual
       ((var ht <ref HashTable>)
        (var key <named LookupKey (template readable mutable)>)
        (var stored_witness <named StoredKey (template readable mutable)>)
        (var out_is_new <ptr i64>)) -> i64
    (block
      (= out_is_new [(: 0 i64)] (: 0 i64))
      (var capacity = (field ht Capacity))
      (var keys = (cast (field ht Keys)
        <ptr <named StoredKey (template readable mutable)>>))
      (var dense_slot = (call rh_lookup_dual
        keys (field ht Dist) (field ht SlotId) capacity key))
      (if (>= dense_slot (: 0 i64)) (block (return dense_slot)))
      (= dense_slot (field ht Size))
      (if (>= dense_slot capacity) (block (return (: -1 i64))))
      (var owned_bytes = (call key_owned_bytes key))
      (var owned_block = (cast (: 0 i64) <ptr u8>))
      (if (> owned_bytes (: 0 i64))
        (block
          (if (! (call aht_owned_blocks_reserve ht (: 1 i64)))
            (block (return (: -1 i64))))
          (= owned_block (cast (call qdb_alloc owned_bytes) <ptr u8>))
          (if (== (cast owned_block i64) (: 0 i64))
            (block (return (: -1 i64))))))
      (var stored_key = (call key_clone_owned key owned_block))
      (if (! (call rh_insert_stored
        keys (field ht Dist) (field ht SlotId) capacity
        stored_key dense_slot))
        (block
          (if (!= (cast owned_block i64) (: 0 i64))
            (block (call qdb_free (cast owned_block <ptr i8>))))
          (return (: -1 i64))))
      (var group_keys = (cast (field ht GroupKeys)
        <ptr <named StoredKey (template readable mutable)>>))
      (= group_keys [dense_slot] stored_key)
      (call aht_owned_blocks_commit ht owned_block)
      (field_assign ht Size (+ dense_slot (: 1 i64)))
      (= out_is_new [(: 0 i64)] (: 1 i64))
      (return dense_slot))))
