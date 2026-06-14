(block
  ;; Symmetric hash join table lifecycle + dense bucket / pair-buffer growth.
  ;; JoinTable and PairBuffer are external types (see modules/qumirdb.cpp).
  ;; The dense per-slot RowId bucket is kept as three parallel arrays
  ;; (BucketCount / BucketCap / BucketData), mirroring AggBuffers, so growth and
  ;; rehash never mutate a struct in place. Concrete i64 storage only — no
  ;; template key here (that arrives with jt_process_batch in E2).

  ;; ---- JoinTable lifecycle (byte-allocating, width-agnostic) ----

  (fun jt_init ((var ht <ref JoinTable>)
                (var capacity i64)
                (var key_size i64)) -> bool
    (block
      (if (|| (< capacity (: 1 i64))
              (> capacity (: 1152921504606846975 i64)))
        (block (return #f)))
      (if (|| (< key_size (: 1 i64))
              (> capacity (/ (: 9223372036854775807 i64) key_size)))
        (block (return #f)))
      (var key_bytes = (* capacity key_size))
      (var meta_bytes = (* capacity (: 8 i64)))
      (var keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var dist = (cast (call qdb_alloc meta_bytes) <ptr i64>))
      (var slot_ids = (cast (call qdb_alloc meta_bytes) <ptr i64>))
      (var group_keys = (cast (call qdb_alloc key_bytes) <ptr u8>))
      (var bucket_count = (cast (call qdb_alloc meta_bytes) <ptr i64>))
      (var bucket_cap = (cast (call qdb_alloc meta_bytes) <ptr i64>))
      (var bucket_data = (cast (call qdb_alloc meta_bytes) <ptr <ptr i64>>))
      (var failed bool)
      (= failed #f)
      (if (== (cast keys i64) (: 0 i64)) (block (= failed #t)))
      (if (== (cast dist i64) (: 0 i64)) (block (= failed #t)))
      (if (== (cast slot_ids i64) (: 0 i64)) (block (= failed #t)))
      (if (== (cast group_keys i64) (: 0 i64)) (block (= failed #t)))
      (if (== (cast bucket_count i64) (: 0 i64)) (block (= failed #t)))
      (if (== (cast bucket_cap i64) (: 0 i64)) (block (= failed #t)))
      (if (== (cast bucket_data i64) (: 0 i64)) (block (= failed #t)))
      (if failed
        (block
          (if (!= (cast keys i64) (: 0 i64)) (block (call qdb_free (cast keys <ptr i8>))))
          (if (!= (cast dist i64) (: 0 i64)) (block (call qdb_free (cast dist <ptr i8>))))
          (if (!= (cast slot_ids i64) (: 0 i64)) (block (call qdb_free (cast slot_ids <ptr i8>))))
          (if (!= (cast group_keys i64) (: 0 i64)) (block (call qdb_free (cast group_keys <ptr i8>))))
          (if (!= (cast bucket_count i64) (: 0 i64)) (block (call qdb_free (cast bucket_count <ptr i8>))))
          (if (!= (cast bucket_cap i64) (: 0 i64)) (block (call qdb_free (cast bucket_cap <ptr i8>))))
          (if (!= (cast bucket_data i64) (: 0 i64)) (block (call qdb_free (cast bucket_data <ptr i8>))))
          (return #f)))
      (var i i64)
      (= i (: 0 i64))
      (while (< i capacity)
        (block
          (= dist [i] (: -1 i64))
          (= slot_ids [i] (: -1 i64))
          (= bucket_count [i] (: 0 i64))
          (= bucket_cap [i] (: 0 i64))
          (= bucket_data [i] (cast (: 0 i64) <ptr i64>))
          (= i (+ i (: 1 i64)))))
      (field_assign ht Keys keys)
      (field_assign ht Dist dist)
      (field_assign ht SlotId slot_ids)
      (field_assign ht GroupKeys group_keys)
      (field_assign ht BucketCount bucket_count)
      (field_assign ht BucketCap bucket_cap)
      (field_assign ht BucketData bucket_data)
      (field_assign ht Capacity capacity)
      (field_assign ht Size (: 0 i64))
      (field_assign ht KeySize key_size)
      (return #t)))

  (fun jt_destroy ((var ht <ref JoinTable>))
    (block
      (var capacity = (field ht Capacity))
      (var bucket_data = (field ht BucketData))
      (if (!= (cast bucket_data i64) (: 0 i64))
        (block
          (var i i64)
          (= i (: 0 i64))
          (while (< i capacity)
            (block
              (var data = (index bucket_data i))
              (if (!= (cast data i64) (: 0 i64))
                (block (call qdb_free (cast data <ptr i8>))))
              (= i (+ i (: 1 i64)))))))
      (if (!= (cast (field ht Keys) i64) (: 0 i64)) (block (call qdb_free (cast (field ht Keys) <ptr i8>))))
      (if (!= (cast (field ht Dist) i64) (: 0 i64)) (block (call qdb_free (cast (field ht Dist) <ptr i8>))))
      (if (!= (cast (field ht SlotId) i64) (: 0 i64)) (block (call qdb_free (cast (field ht SlotId) <ptr i8>))))
      (if (!= (cast (field ht GroupKeys) i64) (: 0 i64)) (block (call qdb_free (cast (field ht GroupKeys) <ptr i8>))))
      (if (!= (cast (field ht BucketCount) i64) (: 0 i64)) (block (call qdb_free (cast (field ht BucketCount) <ptr i8>))))
      (if (!= (cast (field ht BucketCap) i64) (: 0 i64)) (block (call qdb_free (cast (field ht BucketCap) <ptr i8>))))
      (if (!= (cast bucket_data i64) (: 0 i64)) (block (call qdb_free (cast bucket_data <ptr i8>))))
      (field_assign ht Keys (cast (: 0 i64) <ptr u8>))
      (field_assign ht Dist (cast (: 0 i64) <ptr i64>))
      (field_assign ht SlotId (cast (: 0 i64) <ptr i64>))
      (field_assign ht GroupKeys (cast (: 0 i64) <ptr u8>))
      (field_assign ht BucketCount (cast (: 0 i64) <ptr i64>))
      (field_assign ht BucketCap (cast (: 0 i64) <ptr i64>))
      (field_assign ht BucketData (cast (: 0 i64) <ptr <ptr i64>>))
      (field_assign ht Capacity (: 0 i64))
      (field_assign ht Size (: 0 i64))))

  ;; ---- dense RowId bucket growth (amortized O(1), x2 from capacity 4) ----

  (fun jb_append ((var ht <ref JoinTable>)
                  (var slot i64)
                  (var row_id i64)) -> bool
    (block
      (var counts = (field ht BucketCount))
      (var caps = (field ht BucketCap))
      (var datas = (field ht BucketData))
      (var count = (index counts slot))
      (var capacity = (index caps slot))
      (var data = (index datas slot))
      (if (== count capacity)
        (block
          (var new_cap i64)
          (if (== capacity (: 0 i64))
            (block (= new_cap (: 4 i64)))
            (block (= new_cap (* capacity (: 2 i64)))))
          (var new_data =
            (cast (call qdb_realloc (cast data <ptr i8>) (* new_cap (: 8 i64))) <ptr i64>))
          (if (== (cast new_data i64) (: 0 i64)) (block (return #f)))
          (= data new_data)
          (= capacity new_cap)
          (= caps [slot] capacity)
          (= datas [slot] data)))
      (= data [count] row_id)
      (= counts [slot] (+ count (: 1 i64)))
      (return #t)))

  ;; ---- output pair buffer growth (2 i64 per pair, interleaved) ----

  (fun pb_push ((var buf <ref PairBuffer>)
                (var left_id i64)
                (var right_id i64)) -> bool
    (block
      (var count = (field buf Count))
      (var capacity = (field buf Capacity))
      (var data = (field buf Data))
      (if (== count capacity)
        (block
          (var new_cap i64)
          (if (== capacity (: 0 i64))
            (block (= new_cap (: 4 i64)))
            (block (= new_cap (* capacity (: 2 i64)))))
          (var new_data =
            (cast (call qdb_realloc (cast data <ptr i8>) (* new_cap (: 16 i64))) <ptr i64>))
          (if (== (cast new_data i64) (: 0 i64)) (block (return #f)))
          (= data new_data)
          (= capacity new_cap)
          (field_assign buf Capacity capacity)
          (field_assign buf Data data)))
      (= data [(* count (: 2 i64))] left_id)
      (= data [(+ (* count (: 2 i64)) (: 1 i64))] right_id)
      (field_assign buf Count (+ count (: 1 i64)))
      (return #t)))

  (fun pb_destroy ((var buf <ref PairBuffer>))
    (block
      (var data = (field buf Data))
      (if (!= (cast data i64) (: 0 i64)) (block (call qdb_free (cast data <ptr i8>))))
      (field_assign buf Count (: 0 i64))
      (field_assign buf Capacity (: 0 i64))
      (field_assign buf Data (cast (: 0 i64) <ptr i64>)))))
