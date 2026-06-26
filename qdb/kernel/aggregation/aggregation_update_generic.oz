(block
  (pragma language overloads)

  (fun agg_table_update_full
       ((var keys_ref <ptr <ptr u8>>)
        (var dist_ref <ptr <ptr i64>>)
        (var slot_ids_ref <ptr <ptr i64>>)
        (var group_keys_ref <ptr <ptr u8>>)
        (var agg_buffers_ref <ptr <ptr <ptr i64>>>)
        (var capacity_ref <ptr i64>)
        (var size_ref <ptr i64>)
        (var key_size i64)
        (var num_aggs i64)
        (var key <named Key (template)>)
        (var value i64)
        (var out_is_new <ptr i64>)) -> i64
    (block
      (var capacity = (index capacity_ref (: 0 i64)))
      (var size = (index size_ref (: 0 i64)))
      (var failed bool)
      (= failed #f)
      (if (> (+ size (: 1 i64)) (- capacity (/ capacity (: 4 i64))))
        (block
          (if (> capacity (: 576460752303423487 i64))
            (block (= failed #t))
            (block
              (var new_capacity = (* capacity (: 2 i64)))
              (if (! (call agg_dense_grow group_keys_ref agg_buffers_ref
                        capacity new_capacity size key_size num_aggs key))
                (block (= failed #t))
                (block
                  (if (! (call agg_rehash_key_bytes keys_ref dist_ref
                            slot_ids_ref capacity_ref key_size key
                            new_capacity))
                    (block (= failed #t))
                    (block (= capacity new_capacity)))))))))
      (var dense_slot i64)
      (= dense_slot (: -1 i64))
      (if (! failed)
        (block
          (= dense_slot (call agg_upsert_key_bytes
            (index keys_ref (: 0 i64))
            (index dist_ref (: 0 i64))
            (index slot_ids_ref (: 0 i64))
            capacity size_ref key out_is_new))
          (if (>= dense_slot (: 0 i64))
            (block
              (call agg_dense_update
                (index group_keys_ref (: 0 i64))
                (index agg_buffers_ref (: 0 i64))
                dense_slot key value
                (!= (index out_is_new (: 0 i64)) (: 0 i64)))))))
      (return dense_slot))))
