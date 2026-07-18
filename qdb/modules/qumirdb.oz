(block
  (pragma language overloads)

  (type StringView <struct (Data <ptr u8>) (Size i64)>)
  (type OwnedString <struct (Data <ptr u8>) (Size i64)>)
  (type DATE i32)
  ;; Nullable stores a regular value plus a validity bit. Most operators are
  ;; null-propagating: NULL + 1 -> NULL, NULL < 10 -> NULL, 5 < 10 -> TRUE.
  ;; SQL boolean logic is special and is implemented by Nullable[bool] overloads:
  ;; FALSE AND NULL -> FALSE, TRUE OR NULL -> TRUE, NULL AND TRUE -> NULL,
  ;; NULL OR FALSE -> NULL, NOT NULL -> NULL.
  (type Nullable [T] <struct (Value T) (Valid bool)>)

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
  (fun qdb_free ((var ptr <ptr i8>)) -> void (attrs extern) (block))

  ;; realloc implemented in Oz on top of qdb_alloc/qdb_free: the caller passes
  ;; the number of live bytes to preserve (old_size), so we copy only that much
  ;; instead of the whole old allocation. Word-copy the 8-byte-aligned bulk, then
  ;; a byte tail for any remainder.
  (fun qdb_realloc
       ((var ptr <ptr i8>) (var old_size i64) (var new_size i64)) -> <ptr i8>
    (block
      (if (== (cast ptr i64) (: 0 i64))
        (block (return (call qdb_alloc new_size))))
      (var new_ptr = (call qdb_alloc new_size))
      (if (== (cast new_ptr i64) (: 0 i64))
        (block (return (cast (: 0 i64) <ptr i8>))))
      (var copy = old_size)
      (if (< new_size copy) (block (= copy new_size)))
      (var words = (>> copy (: 3 i64)))
      (var src_words = (cast ptr <ptr i64>))
      (var dst_words = (cast new_ptr <ptr i64>))
      (var i = (: 0 i64))
      (while (< i words)
        (block
          (= dst_words [i] (index src_words i))
          (= i (+ i (: 1 i64)))))
      (var b = (<< words (: 3 i64)))
      (while (< b copy)
        (block
          (= new_ptr [b] (index ptr b))
          (= b (+ b (: 1 i64)))))
      (call qdb_free ptr)
      (return new_ptr)))

  ;; 128-bit multiply (compiler-rt __multi3). LLVM emits calls to this once the
  ;; O3 autovectorizer widens decimal arithmetic; wasm has no native i128, so
  ;; defining it here lets wasm-ld resolve it inside the module instead of
  ;; importing a JS shim (which would cross the wasm->JS boundary per multiply).
  ;; sret ABI: writes the low 128 bits of (a_hi:a_lo)*(b_hi:b_lo) to ret[0..1].
  ;; Computed with 32-bit limbs (u64 shifts are logical) — no i128 needed.
  (fun __multi3
       ((var ret <ptr i64>) (var a_lo i64) (var a_hi i64)
        (var b_lo i64) (var b_hi i64)) -> void (attrs used)
    (block
      (var mask = (: 4294967295 u64))
      (var al = (cast a_lo u64))
      (var bl = (cast b_lo u64))
      (var a0 = (& al mask))
      (var a1 = (>> al (: 32 u64)))
      (var b0 = (& bl mask))
      (var b1 = (>> bl (: 32 u64)))
      (var t00 = (* a0 b0))
      (var t01 = (* a0 b1))
      (var t10 = (* a1 b0))
      (var t11 = (* a1 b1))
      (var mid = (+ (+ (>> t00 (: 32 u64)) (& t01 mask)) (& t10 mask)))
      (var lo = (+ (& t00 mask) (<< (& mid mask) (: 32 u64))))
      (var hi = (+ (+ (+ t11 (>> t01 (: 32 u64))) (>> t10 (: 32 u64)))
                   (>> mid (: 32 u64))))
      (= hi (+ hi (+ (* al (cast b_hi u64)) (* (cast a_hi u64) bl))))
      (= ret [(: 0 i64)] (cast lo i64))
      (= ret [(: 1 i64)] (cast hi i64))))

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

  (fun substr ((var str StringView) (var start i32) (var length i32)) -> StringView
       (attrs (extern qdb_substring)) (block))

  (fun substr ((var str StringView) (var start i64) (var length i64)) -> StringView
      (block (call qdb_substring str (cast start i32) (cast length i32))))

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
  (fun qdb_sql_date ((var date string)) -> DATE (attrs extern (operator "cast")) (block))
  (fun qdb_sql_interval ((var amount string) (var unit string)) -> i32 (attrs extern) (block))

  (fun qdb_date_to_i32 ((var d DATE)) -> i32 (attrs (operator "cast"))
    (block (return (bitcast d i32))))
  (fun qdb_i32_to_date ((var n i32)) -> DATE (attrs (operator "cast"))
    (block (return (bitcast n DATE))))
  (fun qdb_date_add ((var d DATE) (var n i32)) -> DATE (attrs (operator "+"))
    (block (return (bitcast (+ (bitcast d i32) n) DATE))))
  (fun qdb_date_sub ((var d DATE) (var n i32)) -> DATE (attrs (operator "-"))
    (block (return (bitcast (- (bitcast d i32) n) DATE))))

  ;; nullables
  (fun nullable_from_value [T] ((var value T)) -> <named Nullable [T]> (attrs (operator "cast"))
    (block
      (return (cast (struct ((Value value) (Valid #t))) <named Nullable [T]>))))

  (fun nullable_add [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "+"))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (+ (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [T]>))))

  (fun nullable_add_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [T]> (attrs (operator "+"))
    (block (return (+ a (cast b <named Nullable [T]>)))))

  (fun nullable_add_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "+"))
    (block (return (+ (cast a <named Nullable [T]>) b))))

  (fun nullable_sub [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "-"))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (- (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [T]>))))

  (fun nullable_sub_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [T]> (attrs (operator "-"))
    (block (return (- a (cast b <named Nullable [T]>)))))

  (fun nullable_sub_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "-"))
    (block (return (- (cast a <named Nullable [T]>) b))))

  (fun nullable_mul [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "*"))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (* (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [T]>))))

  (fun nullable_mul_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [T]> (attrs (operator "*"))
    (block (return (* a (cast b <named Nullable [T]>)))))

  (fun nullable_mul_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "*"))
    (block (return (* (cast a <named Nullable [T]>) b))))

  (fun nullable_div [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [f64]> (attrs (operator "/"))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (/ (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [f64]>))))

  (fun nullable_div_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [f64]> (attrs (operator "/"))
    (block (return (/ a (cast b <named Nullable [T]>)))))

  (fun nullable_div_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [f64]> (attrs (operator "/"))
    (block (return (/ (cast a <named Nullable [T]>) b))))

  (fun nullable_mod [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [i64]> (attrs (operator "%"))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (% (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [i64]>))))

  (fun nullable_mod_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [i64]> (attrs (operator "%"))
    (block (return (% a (cast b <named Nullable [T]>)))))

  (fun nullable_mod_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [i64]> (attrs (operator "%"))
    (block (return (% (cast a <named Nullable [T]>) b))))

  (fun nullable_neg [T] ((var a <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "neg"))
    (block
      (return (cast
        (if (field a Valid)
          (struct ((Value (- (field a Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [T]>))))

  (fun nullable_lt [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [bool]> (attrs (operator "<"))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (< (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [bool]>))))

  (fun nullable_lt_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [bool]> (attrs (operator "<"))
    (block (return (< a (cast b <named Nullable [T]>)))))

  (fun nullable_lt_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [bool]> (attrs (operator "<"))
    (block (return (< (cast a <named Nullable [T]>) b))))

  (fun nullable_le [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [bool]> (attrs (operator "<="))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (<= (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [bool]>))))

  (fun nullable_le_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [bool]> (attrs (operator "<="))
    (block (return (<= a (cast b <named Nullable [T]>)))))

  (fun nullable_le_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [bool]> (attrs (operator "<="))
    (block (return (<= (cast a <named Nullable [T]>) b))))

  (fun nullable_gt [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [bool]> (attrs (operator ">"))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (> (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [bool]>))))

  (fun nullable_gt_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [bool]> (attrs (operator ">"))
    (block (return (> a (cast b <named Nullable [T]>)))))

  (fun nullable_gt_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [bool]> (attrs (operator ">"))
    (block (return (> (cast a <named Nullable [T]>) b))))

  (fun nullable_ge [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [bool]> (attrs (operator ">="))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (>= (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [bool]>))))

  (fun nullable_ge_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [bool]> (attrs (operator ">="))
    (block (return (>= a (cast b <named Nullable [T]>)))))

  (fun nullable_ge_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [bool]> (attrs (operator ">="))
    (block (return (>= (cast a <named Nullable [T]>) b))))

  (fun nullable_eq [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [bool]> (attrs (operator "=="))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (== (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [bool]>))))

  (fun nullable_eq_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [bool]> (attrs (operator "=="))
    (block (return (== a (cast b <named Nullable [T]>)))))

  (fun nullable_eq_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [bool]> (attrs (operator "=="))
    (block (return (== (cast a <named Nullable [T]>) b))))

  (fun nullable_ne [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [bool]> (attrs (operator "!="))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (!= (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [bool]>))))

  (fun nullable_ne_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [bool]> (attrs (operator "!="))
    (block (return (!= a (cast b <named Nullable [T]>)))))

  (fun nullable_ne_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [bool]> (attrs (operator "!="))
    (block (return (!= (cast a <named Nullable [T]>) b))))

  (fun nullable_bit_and [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "&"))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (& (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [T]>))))

  (fun nullable_bit_and_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [T]> (attrs (operator "&"))
    (block (return (& a (cast b <named Nullable [T]>)))))

  (fun nullable_bit_and_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "&"))
    (block (return (& (cast a <named Nullable [T]>) b))))

  (fun nullable_bit_or [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "|"))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (| (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [T]>))))

  (fun nullable_bit_or_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [T]> (attrs (operator "|"))
    (block (return (| a (cast b <named Nullable [T]>)))))

  (fun nullable_bit_or_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "|"))
    (block (return (| (cast a <named Nullable [T]>) b))))

  (fun nullable_bit_xor [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "^"))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (^ (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [T]>))))

  (fun nullable_bit_xor_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [T]> (attrs (operator "^"))
    (block (return (^ a (cast b <named Nullable [T]>)))))

  (fun nullable_bit_xor_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "^"))
    (block (return (^ (cast a <named Nullable [T]>) b))))

  (fun nullable_shl [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "<<"))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (<< (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [T]>))))

  (fun nullable_shl_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [T]> (attrs (operator "<<"))
    (block (return (<< a (cast b <named Nullable [T]>)))))

  (fun nullable_shl_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "<<"))
    (block (return (<< (cast a <named Nullable [T]>) b))))

  (fun nullable_shr [T] ((var a <named Nullable [T]>)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator ">>"))
    (block
      (return (cast
        (if (&& (field a Valid) (field b Valid))
          (struct ((Value (>> (field a Value) (field b Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [T]>))))

  (fun nullable_shr_rhs [T] ((var a <named Nullable [T]>)
        (var b T)) -> <named Nullable [T]> (attrs (operator ">>"))
    (block (return (>> a (cast b <named Nullable [T]>)))))

  (fun nullable_shr_lhs [T] ((var a T)
        (var b <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator ">>"))
    (block (return (>> (cast a <named Nullable [T]>) b))))

  (fun nullable_bit_not [T] ((var a <named Nullable [T]>)) -> <named Nullable [T]> (attrs (operator "~"))
    (block
      (return (cast
        (if (field a Valid)
          (struct ((Value (~ (field a Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [T]>))))

  (fun nullable_bool_and ((var a <named Nullable [bool]>)
        (var b <named Nullable [bool]>)) -> <named Nullable [bool]> (attrs (operator "&&"))
    (block
      (return (cast
        (if (|| (&& (field a Valid) (! (field a Value)))
                (&& (field b Valid) (! (field b Value))))
          (struct ((Value #f) (Valid #t)))
          (if (&& (field a Valid) (field b Valid))
            (struct ((Value (&& (field a Value) (field b Value))) (Valid #t)))
            (struct ((Valid #f)))))
        <named Nullable [bool]>))))

  (fun nullable_bool_and_rhs ((var a <named Nullable [bool]>)
        (var b bool)) -> <named Nullable [bool]> (attrs (operator "&&"))
    (block (return (&& a (cast b <named Nullable [bool]>)))))

  (fun nullable_bool_and_lhs ((var a bool)
        (var b <named Nullable [bool]>)) -> <named Nullable [bool]> (attrs (operator "&&"))
    (block (return (&& (cast a <named Nullable [bool]>) b))))

  (fun nullable_bool_or ((var a <named Nullable [bool]>)
        (var b <named Nullable [bool]>)) -> <named Nullable [bool]> (attrs (operator "||"))
    (block
      (return (cast
        (if (|| (&& (field a Valid) (field a Value))
                (&& (field b Valid) (field b Value)))
          (struct ((Value #t) (Valid #t)))
          (if (&& (field a Valid) (field b Valid))
            (struct ((Value (|| (field a Value) (field b Value))) (Valid #t)))
            (struct ((Valid #f)))))
        <named Nullable [bool]>))))

  (fun nullable_bool_or_rhs ((var a <named Nullable [bool]>)
        (var b bool)) -> <named Nullable [bool]> (attrs (operator "||"))
    (block (return (|| a (cast b <named Nullable [bool]>)))))

  (fun nullable_bool_or_lhs ((var a bool)
        (var b <named Nullable [bool]>)) -> <named Nullable [bool]> (attrs (operator "||"))
    (block (return (|| (cast a <named Nullable [bool]>) b))))

  (fun nullable_bool_not ((var a <named Nullable [bool]>)) -> <named Nullable [bool]> (attrs (operator "!"))
    (block
      (return (cast
        (if (field a Valid)
          (struct ((Value (! (field a Value))) (Valid #t)))
          (struct ((Valid #f))))
        <named Nullable [bool]>))))

)
