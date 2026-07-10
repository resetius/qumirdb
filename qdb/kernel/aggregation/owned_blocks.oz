(block
  (fun owned_blocks_reserve
       ((var blocks_ref <ptr <ptr <ptr u8>>>)
        (var count_ref <ptr i64>)
        (var capacity_ref <ptr i64>)
        (var additional i64)) -> bool
    (block
      (if (< additional (: 0 i64)) (block (return #f)))
      (var count = (index count_ref (: 0 i64)))
      (var capacity = (index capacity_ref (: 0 i64)))
      (var required = (+ count additional))
      (if (< required count) (block (return #f)))
      (if (> required capacity)
        (block
          (var new_capacity = capacity)
          (if (< new_capacity (: 4 i64))
            (block (= new_capacity (: 4 i64))))
          (while (< new_capacity required)
            (block
              (if (> new_capacity (: 1152921504606846975 i64))
                (block (return #f)))
              (= new_capacity (* new_capacity (: 2 i64)))))
          (var blocks = (index blocks_ref (: 0 i64)))
          (var grown = (cast
            (call qdb_realloc
              (cast blocks <ptr i8>)
              (* count (: 8 i64)) (* new_capacity (: 8 i64)))
            <ptr <ptr u8>>))
          (if (== (cast grown i64) (: 0 i64))
            (block (return #f)))
          (= blocks_ref [(: 0 i64)] grown)
          (= capacity_ref [(: 0 i64)] new_capacity)))
      (return #t)))

  (fun owned_blocks_commit
       ((var blocks_ref <ptr <ptr <ptr u8>>>)
        (var count_ref <ptr i64>)
        (var capacity_ref <ptr i64>)
        (var block <ptr u8>)) -> bool
    (block
      (if (== (cast block i64) (: 0 i64))
        (block (return #t)))
      (var count = (index count_ref (: 0 i64)))
      (var capacity = (index capacity_ref (: 0 i64)))
      (if (>= count capacity) (block (return #f)))
      (var blocks = (index blocks_ref (: 0 i64)))
      (= blocks [count] block)
      (= count_ref [(: 0 i64)] (+ count (: 1 i64)))
      (return #t)))

  (fun owned_blocks_register
       ((var blocks_ref <ptr <ptr <ptr u8>>>)
        (var count_ref <ptr i64>)
        (var capacity_ref <ptr i64>)
        (var block <ptr u8>)) -> bool
    (block
      (if (== (cast block i64) (: 0 i64))
        (block (return #t)))
      (if (! (call owned_blocks_reserve
        blocks_ref count_ref capacity_ref (: 1 i64)))
        (block (return #f)))
      (return (call owned_blocks_commit
        blocks_ref count_ref capacity_ref block))))

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
      (= capacity_ref [(: 0 i64)] (: 0 i64))))

  (fun aht_owned_blocks_reserve
       ((var ht <ref HashTable>) (var additional i64)) -> bool
    (block
      (if (< additional (: 0 i64)) (block (return #f)))
      (var count = (field ht OwnedBlockCount))
      (var capacity = (field ht OwnedBlockCapacity))
      (var required = (+ count additional))
      (if (< required count) (block (return #f)))
      (if (> required capacity)
        (block
          (var new_capacity = capacity)
          (if (< new_capacity (: 4 i64))
            (block (= new_capacity (: 4 i64))))
          (while (< new_capacity required)
            (block
              (if (> new_capacity (: 1152921504606846975 i64))
                (block (return #f)))
              (= new_capacity (* new_capacity (: 2 i64)))))
          (var grown = (cast
            (call qdb_realloc
              (cast (field ht OwnedBlocks) <ptr i8>)
              (* count (: 8 i64)) (* new_capacity (: 8 i64)))
            <ptr <ptr u8>>))
          (if (== (cast grown i64) (: 0 i64))
            (block (return #f)))
          (field_assign ht OwnedBlocks grown)
          (field_assign ht OwnedBlockCapacity new_capacity)))
      (return #t)))

  (fun aht_owned_blocks_commit
       ((var ht <ref HashTable>) (var block <ptr u8>))
    (block
      (if (!= (cast block i64) (: 0 i64))
        (block
          (var count = (field ht OwnedBlockCount))
          (var blocks = (field ht OwnedBlocks))
          (= blocks [count] block)
          (field_assign ht OwnedBlockCount (+ count (: 1 i64))))))))
