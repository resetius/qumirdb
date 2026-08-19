(block
  (pragma language overloads)

  ;; qdb_string_hash_bytes is declared extern in qumirdb.oz and implemented
  ;; natively (qumirdb_runtime.cpp), mirroring arrow::internal::ComputeStringHash.

  (fun qdb_string_equal_bytes ((var left <ptr u8>)
                               (var left_size i64)
                               (var right <ptr u8>)
                               (var right_size i64)) -> bool
    (block
      (var equal = (== left_size right_size))
      (var index i64)
      (= index (: 0 i64))
      (while (&& equal (< index left_size))
        (block
          (= equal (== (index left index) (index right index)))
          (= index (+ index (: 1 i64)))))
      (return equal)))

  (fun qdb_string_compare_bytes ((var left <ptr u8>)
                                 (var left_size i64)
                                 (var right <ptr u8>)
                                 (var right_size i64)) -> i64
    (block
      (var limit i64)
      (= limit right_size)
      (if (< left_size right_size)
        (block (= limit left_size)))
      (var result i64)
      (= result (: 0 i64))
      (var index i64)
      (= index (: 0 i64))
      (while (&& (== result (: 0 i64)) (< index limit))
        (block
          (var left_byte = (index left index))
          (var right_byte = (index right index))
          (if (< left_byte right_byte)
            (block (= result (: -1 i64))))
          (if (> left_byte right_byte)
            (block (= result (: 1 i64))))
          (= index (+ index (: 1 i64)))))
      (if (&& (== result (: 0 i64)) (< left_size right_size))
        (block (= result (: -1 i64))))
      (if (&& (== result (: 0 i64)) (> left_size right_size))
        (block (= result (: 1 i64))))
      (return result)))

  (fun qdb_string_copy_bytes ((var destination <ptr u8>)
                              (var source <ptr u8>)
                              (var size i64)) -> i64
    (block
      (var index i64)
      (= index (: 0 i64))
      (while (< index size)
        (block
          (= destination [index] (index source index))
          (= index (+ index (: 1 i64)))))
      (return size)))

  (fun qdb_string_hash ((var value StringView)) -> i64
    (block
      (return (call qdb_string_hash_bytes
        (field value Data) (field value Size)))))

  (fun qdb_string_hash ((var value OwnedString)) -> i64
    (block
      (return (call qdb_string_hash_bytes
        (field value Data) (field value Size)))))

  (fun qdb_string_equal ((var left StringView)
                         (var right StringView)) -> bool
    (block
      (return (call qdb_string_equal_bytes
        (field left Data) (field left Size)
        (field right Data) (field right Size)))))

  (fun qdb_string_equal ((var left StringView)
                         (var right OwnedString)) -> bool
    (block
      (return (call qdb_string_equal_bytes
        (field left Data) (field left Size)
        (field right Data) (field right Size)))))

  (fun qdb_string_equal ((var left OwnedString)
                         (var right StringView)) -> bool
    (block
      (return (call qdb_string_equal_bytes
        (field left Data) (field left Size)
        (field right Data) (field right Size)))))

  (fun qdb_string_equal ((var left OwnedString)
                         (var right OwnedString)) -> bool
    (block
      (return (call qdb_string_equal_bytes
        (field left Data) (field left Size)
        (field right Data) (field right Size)))))

  (fun qdb_string_compare ((var left StringView)
                           (var right StringView)) -> i64
    (block
      (return (call qdb_string_compare_bytes
        (field left Data) (field left Size)
        (field right Data) (field right Size)))))

  (fun aggregation_string_hash_view ((var value StringView)) -> i64
    (block
      (return (call qdb_string_hash value))))

  (fun aggregation_string_hash_owned ((var value OwnedString)) -> i64
    (block
      (return (call qdb_string_hash value))))

  (fun aggregation_string_equal_view_owned
       ((var left StringView) (var right OwnedString)) -> bool
    (block
      (return (call qdb_string_equal left right)))))
