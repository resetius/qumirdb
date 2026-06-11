(block
  (fun rh_table_init ((var ht <ref HashTable>) (var capacity i64)) -> bool
    (block
      (if (|| (< capacity (: 1 i64))
              (> capacity (: 1152921504606846975 i64)))
        (block
          (return #f)))
      (var bytes = (* capacity (: 8 i64)))
      (var keys = (cast (call qdb_alloc bytes) <ptr i64>))
      (var dist = (cast (call qdb_alloc bytes) <ptr i64>))
      (var slot_ids = (cast (call qdb_alloc bytes) <ptr i64>))
      (if (|| (== (cast keys i64) (: 0 i64))
              (|| (== (cast dist i64) (: 0 i64))
                  (== (cast slot_ids i64) (: 0 i64))))
        (block
          (if (!= (cast keys i64) (: 0 i64))
            (block (call qdb_free (cast keys <ptr i8>))))
          (if (!= (cast dist i64) (: 0 i64))
            (block (call qdb_free (cast dist <ptr i8>))))
          (if (!= (cast slot_ids i64) (: 0 i64))
            (block (call qdb_free (cast slot_ids <ptr i8>))))
          (return #f)))
      (var i i64)
      (= i (: 0 i64))
      (while (< i capacity)
        (block
          (= keys [i] (: 0 i64))
          (= dist [i] (: -1 i64))
          (= slot_ids [i] (: -1 i64))
          (= i (+ i (: 1 i64)))))
      (field_assign ht Keys keys)
      (field_assign ht Dist dist)
      (field_assign ht SlotId slot_ids)
      (field_assign ht Capacity capacity)
      (field_assign ht Size (: 0 i64))
      (field_assign ht NumKeys (: 1 i64))
      (return #t)))

  (fun rh_table_destroy ((var ht <ref HashTable>))
    (block
      (if (!= (cast (field ht Keys) i64) (: 0 i64))
        (block (call qdb_free (cast (field ht Keys) <ptr i8>))))
      (if (!= (cast (field ht Dist) i64) (: 0 i64))
        (block (call qdb_free (cast (field ht Dist) <ptr i8>))))
      (if (!= (cast (field ht SlotId) i64) (: 0 i64))
        (block (call qdb_free (cast (field ht SlotId) <ptr i8>))))
      (field_assign ht Keys (cast (: 0 i64) <ptr i64>))
      (field_assign ht Dist (cast (: 0 i64) <ptr i64>))
      (field_assign ht SlotId (cast (: 0 i64) <ptr i64>))
      (field_assign ht Capacity (: 0 i64))
      (field_assign ht Size (: 0 i64))))

  ;; op=0 initializes, op!=0 destroys. This concrete function is last.
  (fun aggregation_table_lifecycle ((var ht <ref HashTable>)
                                    (var capacity i64)
                                    (var op i64)) -> i64
    (block
      (return
        (if (== op (: 0 i64))
          (cast (call rh_table_init ht capacity) i64)
          (block
            (call rh_table_destroy ht)
            (: 1 i64)))))))
