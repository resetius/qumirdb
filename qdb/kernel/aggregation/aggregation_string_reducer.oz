(block
  ;; String MIN/MAX state uses two regular i64 aggregate buffers:
  ;; data_buf stores the owned byte pointer, while meta_buf packs
  ;; u32 size in the low word and u32 capacity in the high word.
  (fun agg_string_copy_bytes
       ((var destination <ptr u8>) (var source <ptr u8>) (var size i64))
    (block
      (var i = (: 0 i64))
      (while (< i size)
        (block
          (= destination [i] (index source i))
          (= i (+ i (: 1 i64)))))))

  (fun agg_string_reduce
       ((var data_buf <ptr i64>)
        (var meta_buf <ptr i64>)
        (var slot i64)
        (var value StringView)
        (var is_min bool)
        (var seed bool)) -> bool
    (block
      (var value_size = (field value Size))
      (if (|| (< value_size (: 0 i64))
              (> value_size (: 4294967295 i64)))
        (block (return #f)))

      (var meta = (index meta_buf slot))
      (var current_size = (cast
        (& (cast meta u64) (: 4294967295 u64)) i64))
      (var data = (cast (index data_buf slot) <ptr u8>))
      (var replace = seed)
      (if (! seed)
        (block
          (var compared = (call qdb_filter_string_compare
            (field value Data) value_size data current_size))
          (if is_min
            (block (= replace (< compared (: 0 i64))))
            (block (= replace (> compared (: 0 i64)))))))
      (if (! replace) (block (return #t)))

      (var capacity = (cast
        (>> (cast meta u64) (: 32 u64)) i64))
      (var old_data = data)
      (var grew = #f)
      (if (> value_size capacity)
        (block
          (var new_capacity i64)
          (if (> capacity (: 2147483647 i64))
            (block (= new_capacity (: 4294967295 i64)))
            (block (= new_capacity (* capacity (: 2 i64)))))
          (if (< new_capacity value_size)
            (block (= new_capacity value_size)))
          (= data (cast (call qdb_alloc new_capacity) <ptr u8>))
          (if (== (cast data i64) (: 0 i64))
            (block (return #f)))
          (= capacity new_capacity)
          (= grew #t)))

      (call agg_string_copy_bytes data (field value Data) value_size)
      (if (&& grew (!= (cast old_data i64) (: 0 i64)))
        (block (call qdb_free (cast old_data <ptr i8>))))
      (= data_buf [slot] (cast data i64))
      (= meta_buf [slot]
        (cast
          (| (<< (cast capacity u64) (: 32 u64))
             (cast value_size u64))
          i64))
      (return #t)))

  (fun agg_string_cleanup
       ((var data_buf <ptr i64>) (var meta_buf <ptr i64>) (var size i64))
    (block
      (var slot = (: 0 i64))
      (while (< slot size)
        (block
          (var data = (index data_buf slot))
          (if (!= data (: 0 i64))
            (block (call qdb_free (cast data <ptr i8>))))
          (= data_buf [slot] (: 0 i64))
          (= meta_buf [slot] (: 0 i64))
          (= slot (+ slot (: 1 i64)))))))

  (fun agg_string_cleanup_at
       ((var ht <ref HashTable>) (var data_index i64) (var meta_index i64))
    (block
      (var buffers = (field ht AggBuffers))
      (call agg_string_cleanup
        (index buffers data_index) (index buffers meta_index) (field ht Size))))

  (fun agg_string_measure
       ((var meta_buf <ptr i64>) (var size i64)) -> i64
    (block
      (var bytes = (: 0 i64))
      (var slot = (: 0 i64))
      (while (< slot size)
        (block
          (= bytes (+ bytes (cast
            (& (cast (index meta_buf slot) u64) (: 4294967295 u64)) i64)))
          (= slot (+ slot (: 1 i64)))))
      (return bytes)))

  (fun agg_string_finalize
       ((var data_buf <ptr i64>)
        (var meta_buf <ptr i64>)
        (var output <ptr TColumn>)
        (var size i64))
    (block
      (var column = (index output (: 0 i64)))
      (var output_data =
        (cast (cast (field column Data) i64) <ptr u8>))
      (var offsets =
        (cast (cast (field column Offsets) i64) <ptr i64>))
      (= offsets [(: 0 i64)] (: 0 i64))
      (var slot = (: 0 i64))
      (while (< slot size)
        (block
          (var value_size = (cast
            (& (cast (index meta_buf slot) u64) (: 4294967295 u64)) i64))
          (var destination =
            (cast (+ (cast output_data i64) (index offsets slot)) <ptr u8>))
          (call agg_string_copy_bytes destination
            (cast (index data_buf slot) <ptr u8>) value_size)
          (= offsets [(+ slot (: 1 i64))]
            (+ (index offsets slot) value_size))
          (= slot (+ slot (: 1 i64)))))))

  (fun agg_string_finalize_at
       ((var ht <ref HashTable>)
        (var data_index i64)
        (var meta_index i64)
        (var output i64)
        (var size i64))
    (block
      (var buffers = (field ht AggBuffers))
      (call agg_string_finalize
        (index buffers data_index)
        (index buffers meta_index)
        (cast output <ptr TColumn>)
        size)))
)
