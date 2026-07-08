(block
  (pragma language overloads)

  ;; String-key sort over row indices, working directly on a string column:
  ;; `data` is the byte buffer, `offsets` the (i32 or i64, per offset_width)
  ;; offset array; row bytes live at data + (off(row) - off(0)), sized
  ;; off(row+1) - off(row) — the same access pattern the filter/project
  ;; kernels use. The entry receives one 3-word descriptor
  ;; { data, offsets, offset_width } so the composite cascade keeps its
  ;; one-pointer-per-key ABI.
  ;;
  ;; Algorithm (see the C++ prototype in test/test_sort.cpp): build
  ;; { prefix: first 8 bytes as big-endian u64, row } pairs once, stable LSD
  ;; radix over the prefix, then a stable merge tie-break of every run of
  ;; equal prefixes, comparing full strings.
  ;;
  ;; The cascade's `work` buffer doubles as the pair scratch: pairs are two
  ;; u64 words each, and two pair arrays (keys + merge scratch) are needed,
  ;; so a string key requires work >= 32 * n bytes (numeric keys need 4 * n).

  (fun sv_col_offset
       ((var offsets <ptr u8>) (var offset_width i64) (var i i64)) -> i64
    (block
      (if (== offset_width (: 4 i64))
        (block
          (var offsets32 = (cast offsets <ptr i32>))
          (return (cast (index offsets32 i) i64))))
      (var offsets64 = (cast offsets <ptr i64>))
      (return (index offsets64 i))))

  (fun sv_col_begin
       ((var offsets <ptr u8>) (var offset_width i64) (var row i64)) -> i64
    (block
      (return (- (call sv_col_offset offsets offset_width row)
                 (call sv_col_offset offsets offset_width (: 0 i64))))))

  (fun sv_col_size
       ((var offsets <ptr u8>) (var offset_width i64) (var row i64)) -> i64
    (block
      (return (- (call sv_col_offset offsets offset_width (+ row (: 1 i64)))
                 (call sv_col_offset offsets offset_width row)))))

  ;; First 8 bytes as a big-endian u64; short strings are zero-padded.
  (fun sv_make_prefix
       ((var data <ptr u8>)
        (var offsets <ptr u8>)
        (var offset_width i64)
        (var row i64)) -> u64
    (block
      (var begin = (call sv_col_begin offsets offset_width row))
      (var size = (call sv_col_size offsets offset_width row))
      (var limit = size)
      (if (> limit (: 8 i64))
        (block (= limit (: 8 i64))))
      (var prefix = (: 0 u64))
      (var i i64)
      (= i (: 0 i64))
      (while (< i limit)
        (block
          (= prefix (| (<< prefix (: 8 u64))
                       (cast (index data (+ begin i)) u64)))
          (= i (+ i (: 1 i64)))))
      (= prefix (<< prefix (cast (* (: 8 i64) (- (: 8 i64) limit)) u64)))
      (return prefix)))

  ;; memcmp + length tie-break: -1 / 0 / 1.
  (fun sv_sort_compare
       ((var data <ptr u8>)
        (var offsets <ptr u8>)
        (var offset_width i64)
        (var a i64)
        (var b i64)) -> i64
    (block
      (var a_begin = (call sv_col_begin offsets offset_width a))
      (var b_begin = (call sv_col_begin offsets offset_width b))
      (var a_size = (call sv_col_size offsets offset_width a))
      (var b_size = (call sv_col_size offsets offset_width b))
      (var limit = a_size)
      (if (< b_size limit)
        (block (= limit b_size)))
      (var i i64)
      (= i (: 0 i64))
      (while (< i limit)
        (block
          (var a_byte = (cast (index data (+ a_begin i)) i64))
          (var b_byte = (cast (index data (+ b_begin i)) i64))
          (if (< a_byte b_byte)
            (block (return (: -1 i64))))
          (if (> a_byte b_byte)
            (block (return (: 1 i64))))
          (= i (+ i (: 1 i64)))))
      (if (< a_size b_size)
        (block (return (: -1 i64))))
      (if (> a_size b_size)
        (block (return (: 1 i64))))
      (return (: 0 i64))))

  ;; True when the pair at `a` must stay before/at the pair at `b`
  ;; (stable keep-order test over full strings).
  (fun sv_pair_keep_order
       ((var data <ptr u8>)
        (var offsets <ptr u8>)
        (var offset_width i64)
        (var pairs <ptr u64>)
        (var a i64)
        (var b i64)
        (var desc bool)) -> bool
    (block
      (var row_a = (cast (index pairs (+ (* a (: 2 i64)) (: 1 i64))) i64))
      (var row_b = (cast (index pairs (+ (* b (: 2 i64)) (: 1 i64))) i64))
      (var cmp = (call sv_sort_compare data offsets offset_width row_a row_b))
      (if desc
        (block (return (>= cmp (: 0 i64)))))
      (return (<= cmp (: 0 i64)))))

  ;; One stable counting pass over prefix byte `digit` (bits) of the pairs;
  ;; sorts into `work` and copies back (prototype CountSort).
  (fun sv_pair_count_pass
       ((var pairs <ptr u64>)
        (var work <ptr u64>)
        (var counts <ptr u32>)
        (var n i64)
        (var digit i64)
        (var desc bool))
    (block
      (var i i64)
      (= i (: 0 i64))
      (while (< i (: 256 i64))
        (block
          (= counts [i] (: 0 u32))
          (= i (+ i (: 1 i64)))))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (var bucket = (cast (& (>> (index pairs (* i (: 2 i64)))
                                     (cast digit u64))
                                 (: 255 u64)) i64))
          (if desc
            (block (= bucket (- (: 255 i64) bucket))))
          (= counts [bucket] (+ (index counts bucket) (: 1 u32)))
          (= i (+ i (: 1 i64)))))

      (= i (: 1 i64))
      (while (< i (: 256 i64))
        (block
          (= counts [i] (+ (index counts i) (index counts (- i (: 1 i64)))))
          (= i (+ i (: 1 i64)))))

      (= i n)
      (while (> i (: 0 i64))
        (block
          (= i (- i (: 1 i64)))
          (var bucket = (cast (& (>> (index pairs (* i (: 2 i64)))
                                     (cast digit u64))
                                 (: 255 u64)) i64))
          (if desc
            (block (= bucket (- (: 255 i64) bucket))))
          (var place = (cast (- (index counts bucket) (: 1 u32)) i64))
          (= work [(* place (: 2 i64))] (index pairs (* i (: 2 i64))))
          (= work [(+ (* place (: 2 i64)) (: 1 i64))]
             (index pairs (+ (* i (: 2 i64)) (: 1 i64))))
          (= counts [bucket] (cast place u32))))

      (= i (: 0 i64))
      (while (< i (* n (: 2 i64)))
        (block
          (= pairs [i] (index work i))
          (= i (+ i (: 1 i64)))))))

  ;; True when the row at pair position `j` must move past `key_row`
  ;; (strictly-after test; equals stay put, preserving stability).
  (fun sv_pair_shifts
       ((var data <ptr u8>)
        (var offsets <ptr u8>)
        (var offset_width i64)
        (var pairs <ptr u64>)
        (var j i64)
        (var key_row i64)
        (var desc bool)) -> bool
    (block
      (var j_row = (cast (index pairs (+ (* j (: 2 i64)) (: 1 i64))) i64))
      (var cmp = (call sv_sort_compare data offsets offset_width j_row key_row))
      (if desc
        (block (return (< cmp (: 0 i64)))))
      (return (> cmp (: 0 i64)))))

  ;; Stable insertion sort of pairs[lo, hi) by full string compare.
  (fun sv_pair_insertion
       ((var data <ptr u8>)
        (var offsets <ptr u8>)
        (var offset_width i64)
        (var pairs <ptr u64>)
        (var lo i64)
        (var hi i64)
        (var desc bool))
    (block
      (var i = (+ lo (: 1 i64)))
      (while (< i hi)
        (block
          (var key_prefix = (index pairs (* i (: 2 i64))))
          (var key_row = (index pairs (+ (* i (: 2 i64)) (: 1 i64))))
          (var j = (- i (: 1 i64)))
          (var moving = #t)
          (while (&& (>= j lo) moving)
            (block
              (if (call sv_pair_shifts data offsets offset_width
                    pairs j (cast key_row i64) desc)
                (block
                  (= pairs [(* (+ j (: 1 i64)) (: 2 i64))]
                     (index pairs (* j (: 2 i64))))
                  (= pairs [(+ (* (+ j (: 1 i64)) (: 2 i64)) (: 1 i64))]
                     (index pairs (+ (* j (: 2 i64)) (: 1 i64))))
                  (= j (- j (: 1 i64))))
                (block
                  (= moving #f)))))
          (= pairs [(* (+ j (: 1 i64)) (: 2 i64))] key_prefix)
          (= pairs [(+ (* (+ j (: 1 i64)) (: 2 i64)) (: 1 i64))] key_row)
          (= i (+ i (: 1 i64)))))))

  ;; Stable bottom-up merge sort of pairs[lo, hi); `work` holds pairs too.
  (fun sv_pair_merge_range
       ((var data <ptr u8>)
        (var offsets <ptr u8>)
        (var offset_width i64)
        (var pairs <ptr u64>)
        (var work <ptr u64>)
        (var lo i64)
        (var hi i64)
        (var desc bool))
    (block
      (if (<= (- hi lo) (: 32 i64))
        (block
          (call sv_pair_insertion data offsets offset_width pairs lo hi desc)
          (return)))

      (var block_lo = lo)
      (while (< block_lo hi)
        (block
          (var block_hi = (+ block_lo (: 32 i64)))
          (if (> block_hi hi)
            (block (= block_hi hi)))
          (call sv_pair_insertion data offsets offset_width pairs block_lo block_hi desc)
          (= block_lo block_hi)))

      (var width = (: 32 i64))
      (while (< width (- hi lo))
        (block
          (var left = lo)
          (while (< left hi)
            (block
              (var mid = (+ left width))
              (var right = (+ mid width))
              (if (> mid hi)
                (block (= mid hi)))
              (if (> right hi)
                (block (= right hi)))
              (var i = left)
              (var j = mid)
              (var k = left)
              (while (&& (< i mid) (< j right))
                (block
                  (if (call sv_pair_keep_order data offsets offset_width pairs i j desc)
                    (block
                      (= work [(* k (: 2 i64))] (index pairs (* i (: 2 i64))))
                      (= work [(+ (* k (: 2 i64)) (: 1 i64))]
                         (index pairs (+ (* i (: 2 i64)) (: 1 i64))))
                      (= i (+ i (: 1 i64))))
                    (block
                      (= work [(* k (: 2 i64))] (index pairs (* j (: 2 i64))))
                      (= work [(+ (* k (: 2 i64)) (: 1 i64))]
                         (index pairs (+ (* j (: 2 i64)) (: 1 i64))))
                      (= j (+ j (: 1 i64)))))
                  (= k (+ k (: 1 i64)))))
              (while (< i mid)
                (block
                  (= work [(* k (: 2 i64))] (index pairs (* i (: 2 i64))))
                  (= work [(+ (* k (: 2 i64)) (: 1 i64))]
                     (index pairs (+ (* i (: 2 i64)) (: 1 i64))))
                  (= i (+ i (: 1 i64)))
                  (= k (+ k (: 1 i64)))))
              (while (< j right)
                (block
                  (= work [(* k (: 2 i64))] (index pairs (* j (: 2 i64))))
                  (= work [(+ (* k (: 2 i64)) (: 1 i64))]
                     (index pairs (+ (* j (: 2 i64)) (: 1 i64))))
                  (= j (+ j (: 1 i64)))
                  (= k (+ k (: 1 i64)))))
              (var copy = (* left (: 2 i64)))
              (while (< copy (* right (: 2 i64)))
                (block
                  (= pairs [copy] (index work copy))
                  (= copy (+ copy (: 1 i64)))))
              (= left (+ left (* width (: 2 i64))))))
          (= width (* width (: 2 i64)))))))

  ;; Tie-break every run of equal prefixes in pairs[lo, hi).
  (fun sv_pair_tiebreak_runs
       ((var data <ptr u8>)
        (var offsets <ptr u8>)
        (var offset_width i64)
        (var pairs <ptr u64>)
        (var work <ptr u64>)
        (var lo i64)
        (var hi i64)
        (var desc bool))
    (block
      (var run_start = lo)
      (var i = (+ lo (: 1 i64)))
      (while (<= i hi)
        (block
          (var boundary = (== i hi))
          (if (! boundary)
            (block
              (= boundary (!= (index pairs (* run_start (: 2 i64)))
                              (index pairs (* i (: 2 i64)))))))
          (if boundary
            (block
              (if (> (- i run_start) (: 1 i64))
                (block
                  (call sv_pair_merge_range data offsets offset_width
                    pairs work run_start i desc)))
              (= run_start i)))
          (= i (+ i (: 1 i64)))))))

  ;; Entry: stable sort of indices[0, n) by the string column described by
  ;; `view_col` = { data ptr, offsets ptr, offset_width } (three i64 words).
  ;; `work` must hold two pair arrays: 32 * n bytes.
  (fun sort_strings_indices
       ((var view_col <ptr i64>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (var data = (cast (index view_col (: 0 i64)) <ptr u8>))
      (var offsets = (cast (index view_col (: 1 i64)) <ptr u8>))
      (var offset_width = (index view_col (: 2 i64)))
      (var pairs = (cast work <ptr u64>))
      (var pair_work = (cast (+ (cast pairs i64) (* n (: 16 i64))) <ptr u64>))

      (var i i64)
      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row = (cast (index indices i) i64))
          (= pairs [(* i (: 2 i64))]
             (call sv_make_prefix data offsets offset_width row))
          (= pairs [(+ (* i (: 2 i64)) (: 1 i64))] (cast row u64))
          (= i (+ i (: 1 i64)))))

      (var digit = (: 0 i64))
      (while (< digit (: 64 i64))
        (block
          (call sv_pair_count_pass pairs pair_work counts n digit desc)
          (= digit (+ digit (: 8 i64)))))

      (call sv_pair_tiebreak_runs data offsets offset_width
        pairs pair_work (: 0 i64) n desc)

      (= i (: 0 i64))
      (while (< i n)
        (block
          (= indices [i] (cast (index pairs (+ (* i (: 2 i64)) (: 1 i64))) u32))
          (= i (+ i (: 1 i64)))))))

  ;; Nullable stable counting pass over pairs: bucket 0 or 256 holds nulls per
  ;; nulls_first (mirrors radix_nullable.oz).
  (fun sv_pair_count_pass_nullable
       ((var pairs <ptr u64>)
        (var work <ptr u64>)
        (var valid <ptr u8>)
        (var counts <ptr u32>)
        (var n i64)
        (var digit i64)
        (var desc bool)
        (var nulls_first bool))
    (block
      (var i i64)
      (= i (: 0 i64))
      (while (< i (: 257 i64))
        (block
          (= counts [i] (: 0 u32))
          (= i (+ i (: 1 i64)))))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row = (cast (index pairs (+ (* i (: 2 i64)) (: 1 i64))) i64))
          (var bucket i64)
          (if (== (index valid row) (: 0 u8))
            (block
              (if nulls_first
                (block (= bucket (: 0 i64)))
                (block (= bucket (: 256 i64)))))
            (block
              (= bucket (cast (& (>> (index pairs (* i (: 2 i64)))
                                     (cast digit u64))
                                 (: 255 u64)) i64))
              (if desc
                (block (= bucket (- (: 255 i64) bucket))))
              (if nulls_first
                (block (= bucket (+ bucket (: 1 i64)))))))
          (= counts [bucket] (+ (index counts bucket) (: 1 u32)))
          (= i (+ i (: 1 i64)))))

      (= i (: 1 i64))
      (while (< i (: 257 i64))
        (block
          (= counts [i] (+ (index counts i) (index counts (- i (: 1 i64)))))
          (= i (+ i (: 1 i64)))))

      (= i n)
      (while (> i (: 0 i64))
        (block
          (= i (- i (: 1 i64)))
          (var row = (cast (index pairs (+ (* i (: 2 i64)) (: 1 i64))) i64))
          (var bucket i64)
          (if (== (index valid row) (: 0 u8))
            (block
              (if nulls_first
                (block (= bucket (: 0 i64)))
                (block (= bucket (: 256 i64)))))
            (block
              (= bucket (cast (& (>> (index pairs (* i (: 2 i64)))
                                     (cast digit u64))
                                 (: 255 u64)) i64))
              (if desc
                (block (= bucket (- (: 255 i64) bucket))))
              (if nulls_first
                (block (= bucket (+ bucket (: 1 i64)))))))
          (var place = (cast (- (index counts bucket) (: 1 u32)) i64))
          (= work [(* place (: 2 i64))] (index pairs (* i (: 2 i64))))
          (= work [(+ (* place (: 2 i64)) (: 1 i64))]
             (index pairs (+ (* i (: 2 i64)) (: 1 i64))))
          (= counts [bucket] (cast place u32))))

      (= i (: 0 i64))
      (while (< i (* n (: 2 i64)))
        (block
          (= pairs [i] (index work i))
          (= i (+ i (: 1 i64)))))))

  ;; Nullable entry: nulls are grouped at the chosen end by the passes; the
  ;; tie-break runs only over the contiguous valid region.
  (fun sort_strings_indices_nullable
       ((var view_col <ptr i64>)
        (var valid <ptr u8>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool)
        (var nulls_first bool))
    (block
      (var data = (cast (index view_col (: 0 i64)) <ptr u8>))
      (var offsets = (cast (index view_col (: 1 i64)) <ptr u8>))
      (var offset_width = (index view_col (: 2 i64)))
      (var pairs = (cast work <ptr u64>))
      (var pair_work = (cast (+ (cast pairs i64) (* n (: 16 i64))) <ptr u64>))

      (var i i64)
      (= i (: 0 i64))
      (while (< i n)
        (block
          (var row = (cast (index indices i) i64))
          (var prefix = (: 0 u64))
          (if (!= (index valid row) (: 0 u8))
            (block
              (= prefix (call sv_make_prefix data offsets offset_width row))))
          (= pairs [(* i (: 2 i64))] prefix)
          (= pairs [(+ (* i (: 2 i64)) (: 1 i64))] (cast row u64))
          (= i (+ i (: 1 i64)))))

      (var digit = (: 0 i64))
      (while (< digit (: 64 i64))
        (block
          (call sv_pair_count_pass_nullable
            pairs pair_work valid counts n digit desc nulls_first)
          (= digit (+ digit (: 8 i64)))))

      (var lo = (: 0 i64))
      (var hi = n)
      (var scanning = #t)
      (while (&& (< lo hi) scanning)
        (block
          (if (== (index valid
                    (cast (index pairs (+ (* lo (: 2 i64)) (: 1 i64))) i64))
                  (: 0 u8))
            (block (= lo (+ lo (: 1 i64))))
            (block (= scanning #f)))))
      (= scanning #t)
      (while (&& (< lo hi) scanning)
        (block
          (if (== (index valid
                    (cast (index pairs (+ (* (- hi (: 1 i64)) (: 2 i64)) (: 1 i64))) i64))
                  (: 0 u8))
            (block (= hi (- hi (: 1 i64))))
            (block (= scanning #f)))))
      (call sv_pair_tiebreak_runs data offsets offset_width
        pairs pair_work lo hi desc)

      (= i (: 0 i64))
      (while (< i n)
        (block
          (= indices [i] (cast (index pairs (+ (* i (: 2 i64)) (: 1 i64))) u32))
          (= i (+ i (: 1 i64)))))))
)
