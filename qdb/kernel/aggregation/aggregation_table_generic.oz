(block
  (pragma language overloads)

  ;; Allocates opaque key bytes plus the fixed-width probe metadata. The
  ;; caller owns only untyped pointers and scalar metadata.
  (fun agg_table_init_bytes ((var keys_out <ptr <ptr u8>>)
                             (var dist_out <ptr <ptr i64>>)
                             (var slot_ids_out <ptr <ptr i64>>)
                             (var capacity_out <ptr i64>)
                             (var size_out <ptr i64>)
                             (var capacity i64)
                             (var key_size i64)) -> bool
    (block
      (if (|| (< capacity (: 1 i64))
              (|| (< key_size (: 1 i64))
                  (|| (> capacity (/ (: 9223372036854775807 i64) key_size))
                      (> capacity (: 1152921504606846975 i64)))))
        (block (return #f)))
      (var key_bytes = (* capacity key_size))
      (var metadata_bytes = (* capacity (: 8 i64)))
      (var keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var dist = (cast (call qdb_alloc metadata_bytes) <ptr i64>))
      (var slot_ids = (cast (call qdb_alloc metadata_bytes) <ptr i64>))
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
      (var index i64)
      (= index (: 0 i64))
      (while (< index capacity)
        (block
          (= dist [index] (: -1 i64))
          (= slot_ids [index] (: -1 i64))
          (= index (+ index (: 1 i64)))))
      (= keys_out [(: 0 i64)] keys)
      (= dist_out [(: 0 i64)] dist)
      (= slot_ids_out [(: 0 i64)] slot_ids)
      (= capacity_out [(: 0 i64)] capacity)
      (= size_out [(: 0 i64)] (: 0 i64))
      (return #t)))

  (fun agg_table_destroy_bytes ((var keys_ref <ptr <ptr u8>>)
                                (var dist_ref <ptr <ptr i64>>)
                                (var slot_ids_ref <ptr <ptr i64>>)
                                (var capacity_ref <ptr i64>)
                                (var size_ref <ptr i64>))
    (block
      (var keys = (index keys_ref (: 0 i64)))
      (var dist = (index dist_ref (: 0 i64)))
      (var slot_ids = (index slot_ids_ref (: 0 i64)))
      (if (!= (cast keys i64) (: 0 i64))
        (block (call qdb_free (cast keys <ptr i8>))))
      (if (!= (cast dist i64) (: 0 i64))
        (block (call qdb_free (cast dist <ptr i8>))))
      (if (!= (cast slot_ids i64) (: 0 i64))
        (block (call qdb_free (cast slot_ids <ptr i8>))))
      (= keys_ref [(: 0 i64)] (cast (: 0 i64) <ptr u8>))
      (= dist_ref [(: 0 i64)] (cast (: 0 i64) <ptr i64>))
      (= slot_ids_ref [(: 0 i64)] (cast (: 0 i64) <ptr i64>))
      (= capacity_ref [(: 0 i64)] (: 0 i64))
      (= size_ref [(: 0 i64)] (: 0 i64))))

  ;; External/runtime storage stays opaque. The concrete key pointer exists
  ;; only inside the specialized query AST.
  (fun agg_upsert_key_bytes [Key] ((var key_bytes <ptr u8>)
                             (var dist <ptr i64>)
                             (var slot_ids <ptr i64>)
                             (var capacity i64)
                             (var size <ptr i64>)
                             (var key Key)
                             (var out_is_new <ptr i64>)) -> i64
    (block
      (var typed_keys =
        (cast key_bytes <ptr Key>))
      (return (call rh_upsert typed_keys dist slot_ids capacity size key
                    out_is_new))))

  (fun agg_rehash_key_bytes [Key] ((var keys_ref <ptr <ptr u8>>)
                             (var dist_ref <ptr <ptr i64>>)
                             (var slot_ids_ref <ptr <ptr i64>>)
                             (var capacity_ref <ptr i64>)
                             (var key_size i64)
                             (var key_witness Key)
                             (var new_capacity i64)) -> bool
    (block
      (if (|| (< new_capacity (: 1 i64))
              (|| (< key_size (: 1 i64))
                  (|| (> new_capacity (/ (: 9223372036854775807 i64) key_size))
                      (> new_capacity (: 1152921504606846975 i64)))))
        (block (return #f)))
      (var key_bytes = (* new_capacity key_size))
      (var metadata_bytes = (* new_capacity (: 8 i64)))
      (var new_keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var new_dist = (cast (call qdb_alloc metadata_bytes) <ptr i64>))
      (var new_slot_ids = (cast (call qdb_alloc metadata_bytes) <ptr i64>))
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

      (var old_keys = (index keys_ref (: 0 i64)))
      (var old_dist = (index dist_ref (: 0 i64)))
      (var old_slot_ids = (index slot_ids_ref (: 0 i64)))
      (var typed_old_keys =
        (cast old_keys <ptr Key>))
      (var typed_new_keys =
        (cast new_keys <ptr Key>))
      (if (! (call rh_rehash_into typed_old_keys old_dist old_slot_ids
                    (index capacity_ref (: 0 i64)) typed_new_keys new_dist
                    new_slot_ids new_capacity))
        (block
          (call qdb_free (cast new_keys <ptr i8>))
          (call qdb_free (cast new_dist <ptr i8>))
          (call qdb_free (cast new_slot_ids <ptr i8>))
          (return #f)))

      (= keys_ref [(: 0 i64)] new_keys)
      (= dist_ref [(: 0 i64)] new_dist)
      (= slot_ids_ref [(: 0 i64)] new_slot_ids)
      (= capacity_ref [(: 0 i64)] new_capacity)
      (call qdb_free (cast old_keys <ptr i8>))
      (call qdb_free (cast old_dist <ptr i8>))
      (call qdb_free (cast old_slot_ids <ptr i8>))
      (return #t)))

  ;; Grows at 75% load, then inserts or returns the existing dense slot id.
  (fun agg_upsert_grow_key_bytes [Key] ((var keys_ref <ptr <ptr u8>>)
                                  (var dist_ref <ptr <ptr i64>>)
                                  (var slot_ids_ref <ptr <ptr i64>>)
                                  (var capacity_ref <ptr i64>)
                                  (var size_ref <ptr i64>)
                                  (var key_size i64)
                                  (var key Key)
                                  (var out_is_new <ptr i64>)) -> i64
    (block
      (var capacity = (index capacity_ref (: 0 i64)))
      (var size = (index size_ref (: 0 i64)))
      (var grow_failed bool)
      (= grow_failed #f)
      (if (> (+ size (: 1 i64)) (- capacity (/ capacity (: 4 i64))))
        (block
          (if (> capacity (: 576460752303423487 i64))
            (block (= grow_failed #t))
            (block
              (if (! (call agg_rehash_key_bytes keys_ref dist_ref slot_ids_ref
                        capacity_ref key_size key (* capacity (: 2 i64))))
                (block (= grow_failed #t))
                (block (= capacity (index capacity_ref (: 0 i64)))))))))
      (return
        (if grow_failed
          (: -1 i64)
          (call agg_upsert_key_bytes
            (index keys_ref (: 0 i64))
            (index dist_ref (: 0 i64))
            (index slot_ids_ref (: 0 i64))
            capacity size_ref key out_is_new))))))
