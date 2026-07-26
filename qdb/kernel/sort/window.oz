(block
  (pragma language overloads)

  ;; Window column fillers. Given a sorted row-id permutation (row_ids[start ..
  ;; start+n)) and the input store, compute one output column aligned to the
  ;; sorted order. Reuses the sort row-id helpers (sr_row_valid /
  ;; sr_load_fixed_key from sort_rowids.oz) and the sort materialize owner list
  ;; (sort_materialize_record_owner from materialize.oz).

  (fun window_init_i64_output
       ((var n i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)) -> <ptr i64>
    (block
      (var data = (cast (call qdb_alloc (* n (: 8 i64))) <ptr i64>))
      (call sort_materialize_record_owner out data_owner_idx (cast data <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask (cast (: 0 i64) <ptr u8>))
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets (cast (: 0 i64) <ptr i64>))
      (field_assign out_col OffsetWidth (: 0 u8))
      (return data)))

  (fun window_init_fixed_output [Value]
       ((var n i64)
        (var value_bytes i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var witness <ptr Value>)) -> <ptr Value>
    (block
      (var data = (cast (call qdb_alloc (* n value_bytes)) <ptr Value>))
      (call sort_materialize_record_owner out data_owner_idx (cast data <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask (cast (: 0 i64) <ptr u8>))
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets (cast (: 0 i64) <ptr i64>))
      (field_assign out_col OffsetWidth (: 0 u8))
      (return data)))

  ;; sum(x) OVER (PARTITION BY optional_i64 ORDER BY ...) with frame UNBOUNDED
  ;; PRECEDING .. CURRENT ROW. NULL args are skipped (SQL sum ignores nulls);
  ;; the output column is non-null.
  (fun window_fill_prefix_sum_i64
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var arg_col i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64))
    (block
      (var data = (call window_init_i64_output n out_col out data_owner_idx))
      (var witness = (cast (: 0 i64) <ptr i64>))
      (var acc = (: 0 i64))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var row_id = (index row_ids (+ start i)))
          (if (> i (: 0 i64))
            (block
              (var prev_row_id = (index row_ids (+ start (- i (: 1 i64)))))
              (if (! (call window_same_partition
                        store prev_row_id row_id))
                (block (= acc (: 0 i64))))))
          (if (call sr_row_valid store row_id arg_col)
            (block
              (= acc (+ acc (call sr_load_fixed_key store row_id arg_col witness)))))
          (= data [i] acc)
          (= i (+ i (: 1 i64)))))))

  ;; rank() OVER (PARTITION BY optional_i64 ORDER BY fixed-width). Peers with
  ;; the same order key keep the same rank; the next distinct peer gets its
  ;; 1-based row position inside the partition.
  (fun window_fill_rank
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64))
    (block
      (var data = (call window_init_i64_output n out_col out data_owner_idx))
      (var rank = (: 1 i64))
      (var pos = (: 1 i64))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var row_id = (index row_ids (+ start i)))
          (if (> i (: 0 i64))
            (block
              (var prev_row_id = (index row_ids (+ start (- i (: 1 i64)))))
              (if (! (call window_same_partition
                        store prev_row_id row_id))
                (block
                  (= rank (: 1 i64))
                  (= pos (: 1 i64)))
                (block
                  (= pos (+ pos (: 1 i64)))
                  (if (! (call window_same_order
                            store prev_row_id row_id))
                    (block (= rank pos)))))))
          (= data [i] rank)
          (= i (+ i (: 1 i64)))))))

  ;; avg(x) OVER (PARTITION BY ...) with no ORDER BY: compute one value for the
  ;; whole contiguous partition and broadcast it to every row in that partition.
  (fun window_fill_partition_avg_f64 [Value]
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var arg_col i64)
        (var arg_witness <ptr Value>)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64))
    (block
      (var out_witness = (cast (: 0 i64) <ptr f64>))
      (var data =
        (call window_init_fixed_output
          n (: 8 i64) out_col out data_owner_idx out_witness))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var first_row_id = (index row_ids (+ start i)))
          (var run_end = (+ i (: 1 i64)))
          (while (&& (< run_end n)
                     (call window_same_partition
                       store first_row_id (index row_ids (+ start run_end))))
            (block (= run_end (+ run_end (: 1 i64)))))

          (var acc = (: 0.0 f64))
          (var count = (: 0 i64))
          (var j = i)
          (while (< j run_end)
            (block
              (var row_id = (index row_ids (+ start j)))
              (if (call sr_row_valid store row_id arg_col)
                (block
                  (= acc (+ acc
                    (cast (call sr_load_fixed_key
                      store row_id arg_col arg_witness) f64)))
                  (= count (+ count (: 1 i64)))))
              (= j (+ j (: 1 i64)))))

          (var avg = (: 0.0 f64))
          (if (> count (: 0 i64))
            (block (= avg (/ acc (cast count f64)))))
          (= j i)
          (while (< j run_end)
            (block
              (= data [j] avg)
              (= j (+ j (: 1 i64)))))
          (= i run_end)))))

  (fun window_fill_partition_avg_binint
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var arg_col i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64))
    (block
      (var out_witness = (cast (: 0 i64) <ptr BinInt>))
      (var data =
        (call window_init_fixed_output
          n (: 16 i64) out_col out data_owner_idx out_witness))
      (var zero = (cast (struct ((Lo (: 0 u64)) (Hi (: 0 u64)))) BinInt))
      (var arg_witness = (cast (: 0 i64) <ptr BinInt>))
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var first_row_id = (index row_ids (+ start i)))
          (var run_end = (+ i (: 1 i64)))
          (while (&& (< run_end n)
                     (call window_same_partition
                       store first_row_id (index row_ids (+ start run_end))))
            (block (= run_end (+ run_end (: 1 i64)))))

          (var acc = zero)
          (var count = (: 0 i64))
          (var j = i)
          (while (< j run_end)
            (block
              (var row_id = (index row_ids (+ start j)))
              (if (call sr_row_valid store row_id arg_col)
                (block
                  (= acc (+ acc
                    (call sr_load_fixed_key store row_id arg_col arg_witness)))
                  (= count (+ count (: 1 i64)))))
              (= j (+ j (: 1 i64)))))

          (var avg = zero)
          (if (> count (: 0 i64))
            (block (= avg (/ acc count))))
          (= j i)
          (while (< j run_end)
            (block
              (= data [j] avg)
              (= j (+ j (: 1 i64)))))
          (= i run_end)))))
)
