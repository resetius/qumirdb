(block
  (pragma language overloads)

  (type StringView <struct (Data <ptr u8>) (Size i64)>)
  (type OwnedString <struct (Data <ptr u8>) (Size i64)>)

  (type TColumn <struct
    (Data <ptr i8>)
    (DataBitOffset i32)
    (Mask <ptr u8>)
    (MaskBitOffset i32)
    (Offsets <ptr i64>)
    (OffsetWidth u8)>)

  (type TRowSet <struct
    (Columns <ptr TColumn>)
    (ColumnCount i64)
    (RowCount i64)
    (Selection <ptr u8>)
    (Destroy <ptr i64>)
    (Private <ptr i64>)
    (RefCount i64)>)

  (type HashTable <struct
    (Keys <ptr u8>)
    (Dist <ptr i64>)
    (SlotId <ptr i64>)
    (GroupKeys <ptr u8>)
    (AggBuffers <ptr <ptr i64>>)
    (OwnedBlocks <ptr <ptr u8>>)
    (OwnedBlockCount i64)
    (OwnedBlockCapacity i64)
    (Capacity i64)
    (Size i64)
    (NumAggs i64)
    (NumKeys i64)
    (KeySize i64)>)

  (type PairBuffer <struct
    (Count i64)
    (Capacity i64)
    (Data <ptr i64>)>)

  (fun bitoff ((var bitmap <ptr u8>) (var index i64) (var bit_offset i64)) -> bool
    (block
      (var bit_index = (+ index bit_offset))
      (var byte_index = (>> bit_index (: 3 i64)))
      (var bit_pos = (& bit_index (: 7 i64)))
      (var byte = (cast (index bitmap byte_index) i64))
      (return (!= (& (>> byte bit_pos) (: 1 i64)) (: 0 i64)))))

  (fun qdb_alloc ((var size i64)) -> <ptr i8> (attrs extern) (block))
  (fun qdb_realloc ((var ptr <ptr i8>) (var size i64)) -> <ptr i8> (attrs extern) (block))
  (fun qdb_free ((var ptr <ptr i8>)) -> void (attrs extern) (block))

  (fun qdb_filter_string_compare
       ((var left <ptr u8>)
        (var left_size i64)
        (var right <ptr u8>)
        (var right_size i64)) -> i64
       (attrs extern) (block))

  (fun qdb_string_view_sql_like
       ((var str StringView) (var pattern string)) -> i64
       (attrs extern) (block))

  (fun qdb_string_view_cmp_cstr
       ((var data <ptr u8>) (var size i64) (var cstr string)) -> i64
       (attrs extern) (block))

  (fun qdb_cstr_cmp_cstr
       ((var left string) (var right string)) -> i64
       (attrs extern) (block))

  (fun qdb_sv_sv_eq ((var left StringView) (var right StringView)) -> bool (attrs (operator "=="))
    (block (return (== (call qdb_filter_string_compare (field left Data) (field left Size) (field right Data) (field right Size)) (: 0 i64)))))
  (fun qdb_sv_sv_ne ((var left StringView) (var right StringView)) -> bool (attrs (operator "!="))
    (block (return (!= (call qdb_filter_string_compare (field left Data) (field left Size) (field right Data) (field right Size)) (: 0 i64)))))
  (fun qdb_sv_sv_lt ((var left StringView) (var right StringView)) -> bool (attrs (operator "<"))
    (block (return (< (call qdb_filter_string_compare (field left Data) (field left Size) (field right Data) (field right Size)) (: 0 i64)))))
  (fun qdb_sv_sv_le ((var left StringView) (var right StringView)) -> bool (attrs (operator "<="))
    (block (return (<= (call qdb_filter_string_compare (field left Data) (field left Size) (field right Data) (field right Size)) (: 0 i64)))))
  (fun qdb_sv_sv_gt ((var left StringView) (var right StringView)) -> bool (attrs (operator ">"))
    (block (return (> (call qdb_filter_string_compare (field left Data) (field left Size) (field right Data) (field right Size)) (: 0 i64)))))
  (fun qdb_sv_sv_ge ((var left StringView) (var right StringView)) -> bool (attrs (operator ">="))
    (block (return (>= (call qdb_filter_string_compare (field left Data) (field left Size) (field right Data) (field right Size)) (: 0 i64)))))

  (fun qdb_sv_lit_eq ((var left StringView) (var right string)) -> bool (attrs extern (operator "==")) (block))
  (fun qdb_sv_lit_ne ((var left StringView) (var right string)) -> bool (attrs extern (operator "!=")) (block))
  (fun qdb_sv_lit_lt ((var left StringView) (var right string)) -> bool (attrs extern (operator "<")) (block))
  (fun qdb_sv_lit_le ((var left StringView) (var right string)) -> bool (attrs extern (operator "<=")) (block))
  (fun qdb_sv_lit_gt ((var left StringView) (var right string)) -> bool (attrs extern (operator ">")) (block))
  (fun qdb_sv_lit_ge ((var left StringView) (var right string)) -> bool (attrs extern (operator ">=")) (block))

  (fun qdb_lit_sv_eq ((var left string) (var right StringView)) -> bool (attrs extern (operator "==")) (block))
  (fun qdb_lit_sv_ne ((var left string) (var right StringView)) -> bool (attrs extern (operator "!=")) (block))
  (fun qdb_lit_sv_lt ((var left string) (var right StringView)) -> bool (attrs extern (operator "<")) (block))
  (fun qdb_lit_sv_le ((var left string) (var right StringView)) -> bool (attrs extern (operator "<=")) (block))
  (fun qdb_lit_sv_gt ((var left string) (var right StringView)) -> bool (attrs extern (operator ">")) (block))
  (fun qdb_lit_sv_ge ((var left string) (var right StringView)) -> bool (attrs extern (operator ">=")) (block))

  (fun qdb_lit_lit_eq ((var left string) (var right string)) -> bool (attrs extern (operator "==")) (block))
  (fun qdb_lit_lit_ne ((var left string) (var right string)) -> bool (attrs extern (operator "!=")) (block))
  (fun qdb_lit_lit_lt ((var left string) (var right string)) -> bool (attrs extern (operator "<")) (block))
  (fun qdb_lit_lit_le ((var left string) (var right string)) -> bool (attrs extern (operator "<=")) (block))
  (fun qdb_lit_lit_gt ((var left string) (var right string)) -> bool (attrs extern (operator ">")) (block))
  (fun qdb_lit_lit_ge ((var left string) (var right string)) -> bool (attrs extern (operator ">=")) (block))

  (fun qdb_substring ((var str StringView) (var start i32) (var length i32)) -> StringView
       (attrs extern) (block))

  (fun qdb_bitmap_set_valid ((var bitmap <ptr u8>) (var index i64) (var valid bool)) -> void
    (block
      (var byte_index = (>> index (: 3 i64)))
      (var bit = (<< (: 1 i64) (& index (: 7 i64))))
      (var byte = (cast (index bitmap byte_index) i64))
      (if valid
        (block
          (= bitmap [byte_index] (cast (| byte bit) u8)))
        (block
          (= bitmap [byte_index] (cast (& byte (^ bit (: 255 i64))) u8))))))

  (fun qdb_sql_bool_and ((var left i64) (var right i64)) -> i64 (attrs extern) (block))
  (fun qdb_sql_bool_or ((var left i64) (var right i64)) -> i64 (attrs extern) (block))
  (fun qdb_sql_bool_not ((var value i64)) -> i64 (attrs extern) (block))

  (fun qdb_date_year ((var days i32)) -> i32 (attrs extern) (block))
  (fun qdb_sql_date ((var date string)) -> i32 (attrs extern) (block))
  (fun qdb_sql_interval ((var amount string) (var unit string)) -> i32 (attrs extern) (block)))
