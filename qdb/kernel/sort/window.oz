(block
  (pragma language overloads)

  ;; Window column fillers. Given a sorted row-id permutation (row_ids[start ..
  ;; start+n)) and the input store, compute one output column aligned to the
  ;; sorted order. Reuses the sort row-id helpers (sr_row_valid /
  ;; sr_load_fixed_key from sort_rowids.oz) and the sort materialize owner list
  ;; (sort_materialize_record_owner from materialize.oz).

  (fun window_same_order [Value]
       ((var store <ptr TRowSet>)
        (var left_row_id i64)
        (var right_row_id i64)
        (var order_col i64)
        (var witness <ptr Value>)) -> bool
    (block
      (if (< order_col (: 0 i64))
        (block (return #t)))
      (var left_valid = (call sr_row_valid store left_row_id order_col))
      (var right_valid = (call sr_row_valid store right_row_id order_col))
      (if (!= left_valid right_valid)
        (block (return #f)))
      (if (! left_valid)
        (block (return #t)))
      (return (== (call sr_load_fixed_key store left_row_id order_col witness)
                  (call sr_load_fixed_key store right_row_id order_col witness)))))

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
  (fun window_fill_rank [OrderValue]
       ((var store <ptr TRowSet>)
        (var row_ids <ptr i64>)
        (var start i64)
        (var n i64)
        (var order_col i64)
        (var order_witness <ptr OrderValue>)
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
                            store prev_row_id row_id order_col order_witness))
                    (block (= rank pos)))))))
          (= data [i] rank)
          (= i (+ i (: 1 i64)))))))
)
