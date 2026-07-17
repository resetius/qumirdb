(block
  (pragma language overloads)

  (fun rh_key_equal ((var left i64) (var right i64)) -> bool
    (block
      (return (== left right))))

  (fun rh_keys_equal [Key] ((var left Key)
                      (var right Key)) -> bool
    (block
      (return (call rh_key_equal left right))))

  (fun aggregation_generic_key_equal ((var left i64) (var right i64)) -> bool
    (block
      (return (call rh_keys_equal left right)))))
