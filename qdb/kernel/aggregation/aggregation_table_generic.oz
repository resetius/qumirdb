(block
  (pragma language overloads)

  ;; External/runtime storage stays opaque. The concrete key pointer exists
  ;; only inside the specialized query AST.
  (fun agg_upsert_key_bytes ((var key_bytes <ptr i8>)
                             (var dist <ptr i64>)
                             (var slot_ids <ptr i64>)
                             (var capacity i64)
                             (var size <ptr i64>)
                             (var key <named Key (template readable mutable)>)
                             (var out_is_new <ptr i64>)) -> i64
    (block
      (var typed_keys =
        (cast key_bytes <ptr <named Key (template readable mutable)>>))
      (return (call rh_upsert typed_keys dist slot_ids capacity size key
                    out_is_new)))))
