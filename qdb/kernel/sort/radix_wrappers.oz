(block
  (pragma language overloads)
  (use radix)

  (fun qdb_radix_sort_indices_u8
       ((var values <ptr u8>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 8 i64) desc)))

  (fun qdb_radix_sort_indices_i8
       ((var values <ptr i8>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 8 i64) desc)))

  (fun qdb_radix_sort_indices_u16
       ((var values <ptr u16>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 16 i64) desc)))

  (fun qdb_radix_sort_indices_i16
       ((var values <ptr i16>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 16 i64) desc)))

  (fun qdb_radix_sort_indices_u32
       ((var values <ptr u32>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 32 i64) desc)))

  (fun qdb_radix_sort_indices_i32
       ((var values <ptr i32>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 32 i64) desc)))

  (fun qdb_radix_sort_indices_u64
       ((var values <ptr u64>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 64 i64) desc)))

  (fun qdb_radix_sort_indices_i64
       ((var values <ptr i64>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 64 i64) desc)))

  (fun qdb_radix_sort_indices_f64
       ((var values <ptr f64>)
        (var indices <ptr u32>)
        (var work <ptr u32>)
        (var counts <ptr u32>)
        (var n i64)
        (var desc bool))
    (block
      (call radix_sort_indices values indices work counts n (: 64 i64) desc)))

  (fun qdb_radix_key_i32_test ((var value i32)) -> u64
    (block
      (return (call qumir_radix_key value))))

  (fun qdb_radix_key_f64_test ((var value f64)) -> u64
    (block
      (return (call qumir_radix_key value)))))
