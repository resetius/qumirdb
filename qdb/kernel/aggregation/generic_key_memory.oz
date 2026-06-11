(block
  (pragma language overloads)

  (fun rh_read_key ((var keys <ptr <named Key (template readable mutable)>>)
                    (var index i64)) -> <named Key (template readable mutable)>
    (block
      (return (index keys index))))

  (fun rh_write_key ((var keys <ptr <named Key (template readable mutable)>>)
                     (var index i64)
                     (var value <named Key (template readable mutable)>))
    (block
      (= keys [index] value)))

  (fun rh_swap_keys ((var keys <ptr <named Key (template readable mutable)>>)
                     (var left i64)
                     (var right i64))
    (block
      (var tmp = (call rh_read_key keys left))
      (call rh_write_key keys left (call rh_read_key keys right))
      (call rh_write_key keys right tmp)))

  ;; CompileKernel returns the last lowered function. Generic specializations
  ;; are appended after their callers, so keep this wrapper free of generic
  ;; calls until the runner exposes an explicit entry-point API.
  (fun aggregation_generic_key_memory ((var keys <ptr i64>)
                                       (var left i64)
                                       (var right i64)
                                       (var value i64)) -> i64
    (block
      (= keys [left] value)
      (var tmp = (index keys left))
      (= keys [left] (index keys right))
      (= keys [right] tmp)
      (return (index keys left)))))
