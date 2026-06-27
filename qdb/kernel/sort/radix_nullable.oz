(block
  (pragma language overloads)
  (use radix)

  (fun radix_count_sort_indices_nullable
       ((var values <ptr <named Value (template)>>)
        (var valid <ptr u8>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
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
          (var row = (index indices i))
          (var row_index = (cast row i64))
          (var bucket i64)
          (if (== (index valid row_index) (: 0 u8))
            (block
              (if nulls_first
                (block (= bucket (: 0 i64)))
                (block (= bucket (: 256 i64)))))
            (block
              (var digit_bucket = (cast (call qumir_radix_digit
                (call qumir_radix_key (index values row_index)) digit) i64))
              (if desc
                (block
                  (= digit_bucket (- (: 255 i64) digit_bucket))))
              (if nulls_first
                (block (= bucket (+ digit_bucket (: 1 i64))))
                (block (= bucket digit_bucket)))))
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
          (var row = (index indices i))
          (var row_index = (cast row i64))
          (var bucket i64)
          (if (== (index valid row_index) (: 0 u8))
            (block
              (if nulls_first
                (block (= bucket (: 0 i64)))
                (block (= bucket (: 256 i64)))))
            (block
              (var digit_bucket = (cast (call qumir_radix_digit
                (call qumir_radix_key (index values row_index)) digit) i64))
              (if desc
                (block
                  (= digit_bucket (- (: 255 i64) digit_bucket))))
              (if nulls_first
                (block (= bucket (+ digit_bucket (: 1 i64))))
                (block (= bucket digit_bucket)))))
          (var place = (- (index counts bucket) (: 1 u32)))
          (= work [(cast place i64)] row)
          (= counts [bucket] place)))

      (= i (: 0 i64))
      (while (< i n)
        (block
          (= indices [i] (index work i))
          (= i (+ i (: 1 i64)))))))

  (fun radix_sort_indices_nullable
       ((var values <ptr <named Value (template)>>)
        (var valid <ptr u8>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var key_bits i64)
        (var desc bool)
        (var nulls_first bool))
    (block
      (var digit i64)
      (= digit (: 0 i64))
      (while (< digit key_bits)
        (block
          (call radix_count_sort_indices_nullable
            values valid indices work counts n digit desc nulls_first)
          (= digit (+ digit (: 8 i64)))))))
)
