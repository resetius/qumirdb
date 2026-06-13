(block
  (fun owned_blocks_register
       ((var blocks_ref <ptr <ptr <ptr u8>>>)
        (var count_ref <ptr i64>)
        (var capacity_ref <ptr i64>)
        (var block <ptr u8>)) -> bool
    (block
      (if (== (cast block i64) (: 0 i64))
        (block (return #t)))
      (var count = (index count_ref (: 0 i64)))
      (var capacity = (index capacity_ref (: 0 i64)))
      (if (== count capacity)
        (block
          (var new_capacity i64)
          (= new_capacity (* capacity (: 2 i64)))
          (if (< new_capacity (: 4 i64))
            (block (= new_capacity (: 4 i64))))
          (var blocks = (index blocks_ref (: 0 i64)))
          (var grown = (cast
            (call qdb_realloc
              (cast blocks <ptr i8>) (* new_capacity (: 8 i64)))
            <ptr <ptr u8>>))
          (if (== (cast grown i64) (: 0 i64))
            (block (return #f)))
          (= blocks_ref [(: 0 i64)] grown)
          (= capacity_ref [(: 0 i64)] new_capacity)))
      (var blocks = (index blocks_ref (: 0 i64)))
      (= blocks [count] block)
      (= count_ref [(: 0 i64)] (+ count (: 1 i64)))
      (return #t)))

  (fun owned_blocks_destroy
       ((var blocks_ref <ptr <ptr <ptr u8>>>)
        (var count_ref <ptr i64>)
        (var capacity_ref <ptr i64>))
    (block
      (var blocks = (index blocks_ref (: 0 i64)))
      (var count = (index count_ref (: 0 i64)))
      (var index i64)
      (= index (: 0 i64))
      (while (< index count)
        (block
          (call qdb_free (cast (index blocks index) <ptr i8>))
          (= index (+ index (: 1 i64)))))
      (if (!= (cast blocks i64) (: 0 i64))
        (block (call qdb_free (cast blocks <ptr i8>))))
      (= blocks_ref [(: 0 i64)] (cast (: 0 i64) <ptr <ptr u8>>))
      (= count_ref [(: 0 i64)] (: 0 i64))
      (= capacity_ref [(: 0 i64)] (: 0 i64)))))
