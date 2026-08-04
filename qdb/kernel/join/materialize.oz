(block
  (pragma language overloads)

  (fun jm_record_owner
       ((var out <ref TRowSet>) (var owner_idx i64) (var ptr <ptr i8>))
    (block
      (var owners = (cast (field out Private) <ptr i64>))
      (= owners [owner_idx] (cast ptr i64))))

  (fun jm_begin
       ((var column_count i64)
        (var owned_ptr_count i64)
        (var n i64)
        (var out <ref TRowSet>)) -> <ptr TColumn>
    (block
      (var alloc_column_count = column_count)
      (if (< alloc_column_count (: 1 i64))
        (block (= alloc_column_count (: 1 i64))))
      (var owners =
        (cast (call qdb_alloc (* (+ owned_ptr_count (: 1 i64)) (: 8 i64)))
              <ptr i64>))
      (var columns =
        (cast (call qdb_alloc (* alloc_column_count (: 48 i64)))
              <ptr TColumn>))
      (= owners [(: 0 i64)] owned_ptr_count)
      (= owners [(: 1 i64)] (cast columns i64))
      (field_assign out Columns columns)
      (field_assign out ColumnCount column_count)
      (field_assign out RowCount n)
      (field_assign out Selection (cast (: 0 i64) <ptr u8>))
      (field_assign out Destroy (cast (: 0 i64) <ptr i64>))
      (field_assign out Private owners)
      (field_assign out RefCount (: 1 i64))
      (return columns)))

  (fun jm_mask_bytes ((var n i64)) -> i64
    (block
      (return (>> (+ n (: 7 i64)) (: 3 i64)))))

  (fun jt_materialize_fixed_column [Value]
       ((var pairs <ref PairBuffer>)
        (var store <ptr TRowSet>)
        (var stream <ref TRowSet>)
        (var start i64)
        (var n i64)
        (var pair_offset i64)
        (var src_col_idx i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64)
        (var value_bytes i64)
        (var type_witness <ptr Value>))
    (block
      (var data = (cast (call qdb_alloc (* n value_bytes)) <ptr Value>))
      (var mask = (cast (call qdb_alloc (call jm_mask_bytes n)) <ptr u8>))
      (call jm_record_owner out data_owner_idx (cast data <ptr i8>))
      (call jm_record_owner out mask_owner_idx (cast mask <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask mask)
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets (cast (: 0 i64) <ptr i64>))
      (field_assign out_col OffsetWidth (: 0 u8))

      (var pair_data = (field pairs Data))
      (var i i64)
      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row_id =
            (index pair_data (+ (* (+ start i) (: 2 i64)) pair_offset)))
          (var valid = #f)
          (var value = (cast (: 0 i64) Value))
          (if (!= row_id (: -1 i64))
            (block
              (var batch_idx = (>> row_id (: 32 i64)))
              (var row = (& row_id (: 4294967295 i64)))
              (var src_columns <ptr TColumn>)
              (if (== batch_idx (: -1 i64))
                (block (= src_columns (field stream Columns)))
                (block (= src_columns (field (index store batch_idx) Columns))))
              (var src_col = (index src_columns src_col_idx))
              (= valid
                (|| (== (cast (field src_col Mask) i64) (: 0 i64))
                    (call bitoff (field src_col Mask) row
                      (cast (field src_col MaskBitOffset) i64))))
              (if valid
                (block
                  (var src_data =
                    (cast (cast (field src_col Data) i64) <ptr Value>))
                  (= value (index src_data row))))))
          (= data [i] value)
          (call qdb_bitmap_set_valid mask i valid)
          (= i (+ i (: 1 i64)))))))

  (fun jt_materialize_binint_column
       ((var pairs <ref PairBuffer>)
        (var store <ptr TRowSet>)
        (var stream <ref TRowSet>)
        (var start i64)
        (var n i64)
        (var pair_offset i64)
        (var src_col_idx i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64))
    (block
      (var data = (cast (call qdb_alloc (* n (: 16 i64))) <ptr BinInt>))
      (var mask = (cast (call qdb_alloc (call jm_mask_bytes n)) <ptr u8>))
      (call jm_record_owner out data_owner_idx (cast data <ptr i8>))
      (call jm_record_owner out mask_owner_idx (cast mask <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask mask)
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets (cast (: 0 i64) <ptr i64>))
      (field_assign out_col OffsetWidth (: 0 u8))

      (var pair_data = (field pairs Data))
      (var zero = (cast (struct ((Lo (: 0 u64)) (Hi (: 0 u64)))) BinInt))
      (var i i64)
      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row_id =
            (index pair_data (+ (* (+ start i) (: 2 i64)) pair_offset)))
          (var valid = #f)
          (var value = zero)
          (if (!= row_id (: -1 i64))
            (block
              (var batch_idx = (>> row_id (: 32 i64)))
              (var row = (& row_id (: 4294967295 i64)))
              (var src_columns <ptr TColumn>)
              (if (== batch_idx (: -1 i64))
                (block (= src_columns (field stream Columns)))
                (block (= src_columns (field (index store batch_idx) Columns))))
              (var src_col = (index src_columns src_col_idx))
              (= valid
                (|| (== (cast (field src_col Mask) i64) (: 0 i64))
                    (call bitoff (field src_col Mask) row
                      (cast (field src_col MaskBitOffset) i64))))
              (if valid
                (block
                  (var src_data =
                    (cast (cast (field src_col Data) i64) <ptr BinInt>))
                  (= value (index src_data row))))))
          (= data [i] value)
          (call qdb_bitmap_set_valid mask i valid)
          (= i (+ i (: 1 i64)))))))

  (fun jt_materialize_bool_column
       ((var pairs <ref PairBuffer>)
        (var store <ptr TRowSet>)
        (var stream <ref TRowSet>)
        (var start i64)
        (var n i64)
        (var pair_offset i64)
        (var src_col_idx i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var data_owner_idx i64)
        (var mask_owner_idx i64))
    (block
      (var byte_count = (call jm_mask_bytes n))
      (var data = (cast (call qdb_alloc byte_count) <ptr u8>))
      (var mask = (cast (call qdb_alloc byte_count) <ptr u8>))
      (call jm_record_owner out data_owner_idx (cast data <ptr i8>))
      (call jm_record_owner out mask_owner_idx (cast mask <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask mask)
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets (cast (: 0 i64) <ptr i64>))
      (field_assign out_col OffsetWidth (: 0 u8))

      (var pair_data = (field pairs Data))
      (var i i64)
      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row_id =
            (index pair_data (+ (* (+ start i) (: 2 i64)) pair_offset)))
          (var valid = #f)
          (var value = #f)
          (if (!= row_id (: -1 i64))
            (block
              (var batch_idx = (>> row_id (: 32 i64)))
              (var row = (& row_id (: 4294967295 i64)))
              (var src_columns <ptr TColumn>)
              (if (== batch_idx (: -1 i64))
                (block (= src_columns (field stream Columns)))
                (block (= src_columns (field (index store batch_idx) Columns))))
              (var src_col = (index src_columns src_col_idx))
              (= valid
                (|| (== (cast (field src_col Mask) i64) (: 0 i64))
                    (call bitoff (field src_col Mask) row
                      (cast (field src_col MaskBitOffset) i64))))
              (if valid
                (block
                  (= value (call bitoff
                    (cast (cast (field src_col Data) i64) <ptr u8>)
                    row (cast (field src_col DataBitOffset) i64)))))))
          (call qdb_bitmap_set_valid data i value)
          (call qdb_bitmap_set_valid mask i valid)
          (= i (+ i (: 1 i64)))))))

  (fun jt_materialize_string_column
       ((var pairs <ref PairBuffer>)
        (var store <ptr TRowSet>)
        (var stream <ref TRowSet>)
        (var start i64)
        (var n i64)
        (var pair_offset i64)
        (var src_col_idx i64)
        (var out_col <ref TColumn>)
        (var out <ref TRowSet>)
        (var offsets_owner_idx i64)
        (var data_owner_idx i64)
        (var mask_owner_idx i64))
    (block
      (var offsets =
        (cast (call qdb_alloc (* (+ n (: 1 i64)) (: 8 i64))) <ptr i64>))
      (var mask = (cast (call qdb_alloc (call jm_mask_bytes n)) <ptr u8>))
      (call jm_record_owner out offsets_owner_idx (cast offsets <ptr i8>))
      (call jm_record_owner out mask_owner_idx (cast mask <ptr i8>))
      (= offsets [(: 0 i64)] (: 0 i64))

      (var pair_data = (field pairs Data))
      (var i i64)
      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row_id =
            (index pair_data (+ (* (+ start i) (: 2 i64)) pair_offset)))
          (var len = (: 0 i64))
          (if (!= row_id (: -1 i64))
            (block
              (var batch_idx = (>> row_id (: 32 i64)))
              (var row = (& row_id (: 4294967295 i64)))
              (var src_columns <ptr TColumn>)
              (if (== batch_idx (: -1 i64))
                (block (= src_columns (field stream Columns)))
                (block (= src_columns (field (index store batch_idx) Columns))))
              (var src_col = (index src_columns src_col_idx))
              (var valid =
                (|| (== (cast (field src_col Mask) i64) (: 0 i64))
                    (call bitoff (field src_col Mask) row
                      (cast (field src_col MaskBitOffset) i64))))
              (if valid
                (block
                  (if (== (cast (field src_col OffsetWidth) i64) (: 4 i64))
                    (block
                      (var src_offsets =
                        (cast (cast (field src_col Offsets) i64) <ptr i32>))
                      (= len (- (cast (index src_offsets (+ row (: 1 i64))) i64)
                                (cast (index src_offsets row) i64))))
                    (block
                      (var src_offsets =
                        (cast (cast (field src_col Offsets) i64) <ptr i64>))
                      (= len (- (index src_offsets (+ row (: 1 i64)))
                                (index src_offsets row)))))))))
          (= offsets [(+ i (: 1 i64))] (+ (index offsets i) len))
          (= i (+ i (: 1 i64)))))

      (var data = (cast (call qdb_alloc (index offsets n)) <ptr u8>))
      (call jm_record_owner out data_owner_idx (cast data <ptr i8>))
      (field_assign out_col Data (cast data <ptr i8>))
      (field_assign out_col DataBitOffset (: 0 i32))
      (field_assign out_col Mask mask)
      (field_assign out_col MaskBitOffset (: 0 i32))
      (field_assign out_col Offsets offsets)
      (field_assign out_col OffsetWidth (: 8 u8))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row_id =
            (index pair_data (+ (* (+ start i) (: 2 i64)) pair_offset)))
          (var valid = #f)
          (if (!= row_id (: -1 i64))
            (block
              (var batch_idx = (>> row_id (: 32 i64)))
              (var row = (& row_id (: 4294967295 i64)))
              (var src_columns <ptr TColumn>)
              (if (== batch_idx (: -1 i64))
                (block (= src_columns (field stream Columns)))
                (block (= src_columns (field (index store batch_idx) Columns))))
              (var src_col = (index src_columns src_col_idx))
              (= valid
                (|| (== (cast (field src_col Mask) i64) (: 0 i64))
                    (call bitoff (field src_col Mask) row
                      (cast (field src_col MaskBitOffset) i64))))
              (if valid
                (block
                  (var begin i64)
                  (var len i64)
                  (if (== (cast (field src_col OffsetWidth) i64) (: 4 i64))
                    (block
                      (var src_offsets =
                        (cast (cast (field src_col Offsets) i64) <ptr i32>))
                      (var base = (cast (index src_offsets (: 0 i64)) i64))
                      (= begin (- (cast (index src_offsets row) i64) base))
                      (= len (- (cast (index src_offsets (+ row (: 1 i64))) i64)
                                (cast (index src_offsets row) i64))))
                    (block
                      (var src_offsets =
                        (cast (cast (field src_col Offsets) i64) <ptr i64>))
                      (var base = (index src_offsets (: 0 i64)))
                      (= begin (- (index src_offsets row) base))
                      (= len (- (index src_offsets (+ row (: 1 i64)))
                                (index src_offsets row)))))
                  (var dst =
                    (cast (+ (cast data i64) (index offsets i)) <ptr u8>))
                  (var src =
                    (cast (+ (cast (field src_col Data) i64) begin) <ptr u8>))
                  (var b i64)
                  (= b (: 0 i64))
                  (while (< b len)
                    (block
                      (= dst [b] (index src b))
                      (= b (+ b (: 1 i64)))))))))
          (call qdb_bitmap_set_valid mask i valid)
          (= i (+ i (: 1 i64)))))
)))
