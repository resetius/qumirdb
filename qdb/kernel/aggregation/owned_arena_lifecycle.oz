(block
  ;; Release every allocation registered by the HashTable-owned arena and
  ;; reset the registry.  Keeping this in a small lifecycle library lets all
  ;; HashTable users share the same destruction path without pulling the
  ;; string-key allocator into fixed-key and cross-join kernels.
  (fun owned_arena_destroy
       ((var blocks_ref <ref <ptr <ptr u8>>>)
        (var count_ref <ref i64>)
        (var capacity_ref <ref i64>))
    (block
      (var blocks = blocks_ref)
      (var count = count_ref)
      (var index = 0)
      (while (< index count)
        (block
          (call qdb_free (cast (index blocks index) <ptr i8>))
          (= index (+ index 1))))
      (if (!= (cast blocks i64) 0)
        (block (call qdb_free (cast blocks <ptr i8>))))
      (= blocks_ref (cast 0 <ptr <ptr u8>>))
      (= count_ref 0)
      (= capacity_ref 0))))
