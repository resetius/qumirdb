(block
  (fun aggregation_concrete_key_memory ((var keys <ptr i64>)
                                        (var index i64)
                                        (var value i64)) -> i64
    (block
      (= keys [index] value)
      (return (index keys index)))))
