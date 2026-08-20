(block
  (pragma language overloads)

  ;; SwissTable probing (absl flat_hash_map) over the dual lookup/stored key
  ;; pair. Ctrl holds one byte per slot: 0x80 empty, otherwise H2 = hash & 0x7F.
  ;; Deletion is unsupported, so there are no tombstones and a probe stops at
  ;; the first group containing an empty slot. Groups are 8 slots wide and
  ;; aligned, so a group is one aligned u64 load (swiss_group.oz); capacity is
  ;; a power of two and at least 8, and probing walks whole groups with
  ;; triangular steps, which visits every group exactly once.

  (fun rh_lookup_dual [StoredKey LookupKey]
       ((var keys <ptr StoredKey>)
        (var ctrl <ptr u8>)
        (var slot_ids <ptr i64>)
        (var capacity i64)
        (var key LookupKey)
        (var hash u64)) -> i64
    (block
      (var words = (cast ctrl <ptr u64>))
      (var num_groups = (>> capacity (: 3 i64)))
      (var group_mask = (- num_groups (: 1 i64)))
      (var h2 = (& hash (: 127 u64)))
      (var g = (& (cast (>> hash (: 7 u64)) i64) group_mask))
      (var step i64)
      (= step (: 0 i64))
      (var probes i64)
      (= probes (: 0 i64))
      (while (< probes num_groups)
        (block
          (var word = (index words g))
          (var m = (call swiss_match word h2))
          (while (!= m (: 0 u64))
            (block
              (var slot = (+ (<< g (: 3 i64)) (call swiss_lowest_index m)))
              (if (call rh_key_equal (index keys slot) key)
                (block (return (index slot_ids slot))))
              (= m (& m (- m (: 1 u64))))))
          (if (!= (call swiss_match_empty word) (: 0 u64))
            (block (return (: -1 i64))))
          (= step (+ step (: 1 i64)))
          (= g (& (+ g step) group_mask))
          (= probes (+ probes (: 1 i64)))))
      (return (: -1 i64))))

  ;; Inserts a key known to be absent. Unlike Robin Hood this never moves a
  ;; resident entry: the first empty slot along the probe sequence wins.
  (fun rh_insert_stored [StoredKey]
       ((var keys <ptr StoredKey>)
        (var ctrl <ptr u8>)
        (var slot_ids <ptr i64>)
        (var capacity i64)
        (var key StoredKey)
        (var dense_slot i64)
        (var hash u64)) -> bool
    (block
      (var words = (cast ctrl <ptr u64>))
      (var num_groups = (>> capacity (: 3 i64)))
      (var group_mask = (- num_groups (: 1 i64)))
      (var h2 = (& hash (: 127 u64)))
      (var g = (& (cast (>> hash (: 7 u64)) i64) group_mask))
      (var step i64)
      (= step (: 0 i64))
      (var probes i64)
      (= probes (: 0 i64))
      (while (< probes num_groups)
        (block
          (var empty = (call swiss_match_empty (index words g)))
          (if (!= empty (: 0 u64))
            (block
              (var slot = (+ (<< g (: 3 i64)) (call swiss_lowest_index empty)))
              (= keys [slot] key)
              (= slot_ids [slot] dense_slot)
              (= ctrl [slot] (cast h2 u8))
              (return #t)))
          (= step (+ step (: 1 i64)))
          (= g (& (+ g step) group_mask))
          (= probes (+ probes (: 1 i64)))))
      (return #f)))

  (fun aht_upsert_dual [LookupKey StoredKey]
       ((var ht <ref HashTable>)
        (var key LookupKey)
        (var stored_witness StoredKey)
        (var out_is_new <ref i64>)
        (var hash u64)) -> i64
    (block
      (= out_is_new (: 0 i64))
      (var capacity = (field ht Capacity))
      (var keys = (cast (field ht Keys)
        <ptr StoredKey>))
      (var dense_slot = (call rh_lookup_dual
        keys (field ht Ctrl) (field ht SlotId) capacity key hash))
      (if (>= dense_slot (: 0 i64)) (block (return dense_slot)))
      (= dense_slot (field ht Size))
      (if (> (+ dense_slot (: 1 i64))
             (- capacity (>> capacity (: 3 i64))))
        (block
          (if (> capacity (: 576460752303423487 i64))
            (block (return (: -1 i64))))
          (if (! (call aht_rehash_dual
            ht (* capacity (: 2 i64)) stored_witness))
            (block (return (: -1 i64))))
          (= capacity (field ht Capacity))
          (= keys (cast (field ht Keys)
            <ptr StoredKey>))))
      (var owned_bytes = (call key_owned_bytes key))
      (var owned_block = (cast (: 0 i64) <ptr u8>))
      (if (> owned_bytes (: 0 i64))
        (block
          (= owned_block (call aht_owned_arena_alloc ht owned_bytes))
          (if (== (cast owned_block i64) (: 0 i64))
            (block (return (: -1 i64))))))
      (var stored_key = (call key_clone_owned key owned_block))
      (if (! (call rh_insert_stored
        keys (field ht Ctrl) (field ht SlotId) capacity
        stored_key dense_slot hash))
        (block
          (if (!= (cast owned_block i64) (: 0 i64))
            (block (call aht_owned_arena_rewind
              ht owned_block owned_bytes)))
          (return (: -1 i64))))
      (var group_keys = (cast (field ht GroupKeys)
        <ptr StoredKey>))
      (= group_keys [dense_slot] stored_key)
      (var agg_buffers = (field ht AggBuffers))
      (var agg i64)
      (= agg (: 0 i64))
      (while (< agg (field ht NumAggs))
        (block
          (var agg_buffer = (index agg_buffers agg))
          (= agg_buffer [dense_slot] (: 0 i64))
          (= agg (+ agg (: 1 i64)))))
      (field_assign ht Size (+ dense_slot (: 1 i64)))
      (= out_is_new (: 1 i64))
      (return dense_slot)))

  (fun rh_rehash_stored [StoredKey]
       ((var old_keys <ptr StoredKey>)
        (var old_ctrl <ptr u8>)
        (var old_slot_ids <ptr i64>)
        (var old_capacity i64)
        (var new_keys <ptr StoredKey>)
        (var new_ctrl <ptr u8>)
        (var new_slot_ids <ptr i64>)
        (var new_capacity i64)
        (var stored_witness StoredKey)) -> bool
    (block
      (var index i64)
      (= index (: 0 i64))
      (while (< index new_capacity)
        (block
          (= new_ctrl [index] (: 128 u8))
          (= new_slot_ids [index] (: -1 i64))
          (= index (+ index (: 1 i64)))))
      (= index (: 0 i64))
      (while (< index old_capacity)
        (block
          (if (< (index old_ctrl index) (: 128 u8))
            (block
              (if (! (call rh_insert_stored
                new_keys new_ctrl new_slot_ids new_capacity
                (index old_keys index) (index old_slot_ids index)
                (cast (call rh_hash (index old_keys index)) u64)))
                (block (return #f)))))
          (= index (+ index (: 1 i64)))))
      (return #t)))

  (fun aht_rehash_dual [StoredKey]
       ((var ht <ref HashTable>)
        (var new_capacity i64)
        (var stored_witness StoredKey)) -> bool
    (block
      (var old_capacity = (field ht Capacity))
      (var size = (field ht Size))
      (var key_size = (field ht KeySize))
      (var num_aggs = (field ht NumAggs))
      ;; Groups are 8 slots wide, so capacity must stay a power of two >= 8.
      (if (|| (< new_capacity size) (< new_capacity (: 8 i64)))
        (block (return #f)))
      (if (!= (& new_capacity (- new_capacity (: 1 i64))) (: 0 i64))
        (block (return #f)))
      (if (|| (< key_size (: 1 i64))
              (> new_capacity (/ (: 9223372036854775807 i64) key_size)))
        (block (return #f)))
      (var key_bytes = (* new_capacity key_size))
      (var meta_bytes = (* new_capacity (: 8 i64)))
      (var new_keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var new_ctrl = (cast (call qdb_alloc new_capacity) <ptr u8>))
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
      (if (== (cast new_ctrl i64) (: 0 i64))
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
          (if (!= (cast new_ctrl i64) (: 0 i64))
            (block (call qdb_free (cast new_ctrl <ptr i8>))))
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
        <ptr StoredKey>))
      (var new_keys_typed = (cast new_keys
        <ptr StoredKey>))
      (if (! (call rh_rehash_stored
        old_keys (field ht Ctrl) (field ht SlotId) old_capacity
        new_keys_typed new_ctrl new_slot_ids new_capacity stored_witness))
        (block
          (call qdb_free (cast new_keys <ptr i8>))
          (call qdb_free (cast new_ctrl <ptr i8>))
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
        <ptr StoredKey>))
      (var new_group_keys_typed = (cast new_group_keys
        <ptr StoredKey>))
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
      (call qdb_free (cast (field ht Ctrl) <ptr i8>))
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
      (field_assign ht Ctrl new_ctrl)
      (field_assign ht SlotId new_slot_ids)
      (field_assign ht GroupKeys new_group_keys)
      (field_assign ht AggBuffers new_agg_buffers)
      (field_assign ht Capacity new_capacity)
      (return #t))))
