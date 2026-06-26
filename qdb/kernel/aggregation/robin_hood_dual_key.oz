(block
  (pragma language overloads)

  (fun rh_lookup_dual
       ((var keys <ptr <named StoredKey (template)>>)
        (var dist <ptr i64>)
        (var slot_ids <ptr i64>)
        (var capacity i64)
        (var key <named LookupKey (template)>)) -> i64
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
       ((var keys <ptr <named StoredKey (template)>>)
        (var dist <ptr i64>)
        (var slot_ids <ptr i64>)
        (var capacity i64)
        (var key <named StoredKey (template)>)
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
        (var key <named LookupKey (template)>)
        (var stored_witness <named StoredKey (template)>)
        (var out_is_new <ref i64>)) -> i64
    (block
      (= out_is_new (: 0 i64))
      (var capacity = (field ht Capacity))
      (var keys = (cast (field ht Keys)
        <ptr <named StoredKey (template)>>))
      (var dense_slot = (call rh_lookup_dual
        keys (field ht Dist) (field ht SlotId) capacity key))
      (if (>= dense_slot (: 0 i64)) (block (return dense_slot)))
      (= dense_slot (field ht Size))
      (if (> (+ dense_slot (: 1 i64))
             (- capacity (/ capacity (: 4 i64))))
        (block
          (if (> capacity (: 576460752303423487 i64))
            (block (return (: -1 i64))))
          (if (! (call aht_rehash_dual
            ht (* capacity (: 2 i64)) stored_witness))
            (block (return (: -1 i64))))
          (= capacity (field ht Capacity))
          (= keys (cast (field ht Keys)
            <ptr <named StoredKey (template)>>))))
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
        <ptr <named StoredKey (template)>>))
      (= group_keys [dense_slot] stored_key)
      (var agg_buffers = (field ht AggBuffers))
      (var agg i64)
      (= agg (: 0 i64))
      (while (< agg (field ht NumAggs))
        (block
          (var agg_buffer = (index agg_buffers agg))
          (= agg_buffer [dense_slot] (: 0 i64))
          (= agg (+ agg (: 1 i64)))))
      (call aht_owned_blocks_commit ht owned_block)
      (field_assign ht Size (+ dense_slot (: 1 i64)))
      (= out_is_new (: 1 i64))
      (return dense_slot)))

  (fun rh_rehash_stored
       ((var old_keys <ptr <named StoredKey (template)>>)
        (var old_dist <ptr i64>)
        (var old_slot_ids <ptr i64>)
        (var old_capacity i64)
        (var new_keys <ptr <named StoredKey (template)>>)
        (var new_dist <ptr i64>)
        (var new_slot_ids <ptr i64>)
        (var new_capacity i64)
        (var stored_witness <named StoredKey (template)>)) -> bool
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
          (if (>= (index old_dist index) (: 0 i64))
            (block
              (if (! (call rh_insert_stored
                new_keys new_dist new_slot_ids new_capacity
                (index old_keys index) (index old_slot_ids index)))
                (block (return #f)))))
          (= index (+ index (: 1 i64)))))
      (return #t)))

  (fun aht_rehash_dual
       ((var ht <ref HashTable>)
        (var new_capacity i64)
        (var stored_witness <named StoredKey (template)>)) -> bool
    (block
      (var old_capacity = (field ht Capacity))
      (var size = (field ht Size))
      (var key_size = (field ht KeySize))
      (var num_aggs = (field ht NumAggs))
      (if (|| (< new_capacity size) (< new_capacity (: 1 i64)))
        (block (return #f)))
      (if (|| (< key_size (: 1 i64))
              (> new_capacity (/ (: 9223372036854775807 i64) key_size)))
        (block (return #f)))
      (var key_bytes = (* new_capacity key_size))
      (var meta_bytes = (* new_capacity (: 8 i64)))
      (var new_keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var new_dist = (cast (call qdb_alloc meta_bytes) <ptr i64>))
      (var new_slot_ids = (cast (call qdb_alloc meta_bytes) <ptr i64>))
      (var new_group_keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var new_agg_buffers = (cast (: 0 i64) <ptr <ptr i64>>))
      (if (> num_aggs (: 0 i64))
        (block
          (= new_agg_buffers (cast
            (call qdb_alloc (* num_aggs (: 8 i64))) <ptr <ptr i64>>))))
      (var allocation_failed bool)
      (= allocation_failed #f)
      (if (== (cast new_keys i64) (: 0 i64))
        (block (= allocation_failed #t)))
      (if (== (cast new_dist i64) (: 0 i64))
        (block (= allocation_failed #t)))
      (if (== (cast new_slot_ids i64) (: 0 i64))
        (block (= allocation_failed #t)))
      (if (== (cast new_group_keys i64) (: 0 i64))
        (block (= allocation_failed #t)))
      (if (&& (> num_aggs (: 0 i64))
              (== (cast new_agg_buffers i64) (: 0 i64)))
        (block (= allocation_failed #t)))
      (var allocated_aggs i64)
      (= allocated_aggs (: 0 i64))
      (if (! allocation_failed)
        (block
          (while (&& (! allocation_failed) (< allocated_aggs num_aggs))
            (block
              (= new_agg_buffers [allocated_aggs]
                (cast (call qdb_alloc meta_bytes) <ptr i64>))
              (if (== (cast (index new_agg_buffers allocated_aggs) i64)
                       (: 0 i64))
                (block (= allocation_failed #t)))
              (if (! allocation_failed)
                (block (= allocated_aggs (+ allocated_aggs (: 1 i64)))))))))
      (if allocation_failed
        (block
          (if (!= (cast new_keys i64) (: 0 i64))
            (block (call qdb_free (cast new_keys <ptr i8>))))
          (if (!= (cast new_dist i64) (: 0 i64))
            (block (call qdb_free (cast new_dist <ptr i8>))))
          (if (!= (cast new_slot_ids i64) (: 0 i64))
            (block (call qdb_free (cast new_slot_ids <ptr i8>))))
          (if (!= (cast new_group_keys i64) (: 0 i64))
            (block (call qdb_free (cast new_group_keys <ptr i8>))))
          (var cleanup_agg i64)
          (= cleanup_agg (: 0 i64))
          (while (< cleanup_agg allocated_aggs)
            (block
              (call qdb_free
                (cast (index new_agg_buffers cleanup_agg) <ptr i8>))
              (= cleanup_agg (+ cleanup_agg (: 1 i64)))))
          (if (!= (cast new_agg_buffers i64) (: 0 i64))
            (block (call qdb_free (cast new_agg_buffers <ptr i8>))))
          (return #f)))
      (var old_keys = (cast (field ht Keys)
        <ptr <named StoredKey (template)>>))
      (var new_keys_typed = (cast new_keys
        <ptr <named StoredKey (template)>>))
      (if (! (call rh_rehash_stored
        old_keys (field ht Dist) (field ht SlotId) old_capacity
        new_keys_typed new_dist new_slot_ids new_capacity stored_witness))
        (block
          (call qdb_free (cast new_keys <ptr i8>))
          (call qdb_free (cast new_dist <ptr i8>))
          (call qdb_free (cast new_slot_ids <ptr i8>))
          (call qdb_free (cast new_group_keys <ptr i8>))
          (var cleanup_agg i64)
          (= cleanup_agg (: 0 i64))
          (while (< cleanup_agg num_aggs)
            (block
              (call qdb_free
                (cast (index new_agg_buffers cleanup_agg) <ptr i8>))
              (= cleanup_agg (+ cleanup_agg (: 1 i64)))))
          (if (!= (cast new_agg_buffers i64) (: 0 i64))
            (block (call qdb_free (cast new_agg_buffers <ptr i8>))))
          (return #f)))
      (var old_group_keys = (cast (field ht GroupKeys)
        <ptr <named StoredKey (template)>>))
      (var new_group_keys_typed = (cast new_group_keys
        <ptr <named StoredKey (template)>>))
      (var index i64)
      (= index (: 0 i64))
      (while (< index size)
        (block
          (= new_group_keys_typed [index] (index old_group_keys index))
          (= index (+ index (: 1 i64)))))
      (var old_agg_buffers = (field ht AggBuffers))
      (var agg i64)
      (= agg (: 0 i64))
      (while (< agg num_aggs)
        (block
          (var old_buffer = (index old_agg_buffers agg))
          (var new_buffer = (index new_agg_buffers agg))
          (= index (: 0 i64))
          (while (< index size)
            (block
              (= new_buffer [index] (index old_buffer index))
              (= index (+ index (: 1 i64)))))
          (= agg (+ agg (: 1 i64)))))
      (call qdb_free (cast (field ht Keys) <ptr i8>))
      (call qdb_free (cast (field ht Dist) <ptr i8>))
      (call qdb_free (cast (field ht SlotId) <ptr i8>))
      (call qdb_free (cast (field ht GroupKeys) <ptr i8>))
      (= agg (: 0 i64))
      (while (< agg num_aggs)
        (block
          (call qdb_free (cast (index old_agg_buffers agg) <ptr i8>))
          (= agg (+ agg (: 1 i64)))))
      (if (!= (cast old_agg_buffers i64) (: 0 i64))
        (block (call qdb_free (cast old_agg_buffers <ptr i8>))))
      (field_assign ht Keys new_keys)
      (field_assign ht Dist new_dist)
      (field_assign ht SlotId new_slot_ids)
      (field_assign ht GroupKeys new_group_keys)
      (field_assign ht AggBuffers new_agg_buffers)
      (field_assign ht Capacity new_capacity)
      (return #t))))
