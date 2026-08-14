(block
  ;; Grow the registry that owns arena chunks.  This helper reserves pointer
  ;; slots only; owned_arena_alloc is the sole production path that commits a
  ;; chunk, after initializing its tagged header.
  (fun owned_arena_reserve_chunks
       ((var blocks_ref <ref <ptr <ptr u8>>>)
        (var count_ref <ref i64>)
        (var capacity_ref <ref i64>)
        (var additional i64)) -> bool
    (block
      (if (< additional 0) (block (return #f)))
      (var count = count_ref)
      (var capacity = capacity_ref)
      (var required = (+ count additional))
      (if (< required count) (block (return #f)))
      (if (> required capacity)
        (block
          (var new_capacity = capacity)
          (if (< new_capacity 4)
            (block (= new_capacity 4)))
          (while (< new_capacity required)
            (block
              (if (> new_capacity 1152921504606846975)
                (block (return #f)))
              (= new_capacity (* new_capacity 2))))
          (var blocks = blocks_ref)
          (var grown = (cast
            (call qdb_realloc
              (cast blocks <ptr i8>)
              (* count 8) (* new_capacity 8))
            <ptr <ptr u8>>))
          (if (== (cast grown i64) 0)
            (block (return #f)))
          (= blocks_ref grown)
          (= capacity_ref new_capacity)))
      (return #t)))

  ;; String-bearing group keys live in geometrically grown chunks.  OwnedBlocks
  ;; owns chunk bases for the lifetime of the HashTable.  Every arena chunk has
  ;; a {magic, capacity, used} header; the tag prevents a legacy or foreign
  ;; registry entry from being interpreted as allocator state.  Callers receive
  ;; byte slices after the 24-byte header.
  (fun owned_arena_alloc
       ((var blocks_ref <ref <ptr <ptr u8>>>)
        (var count_ref <ref i64>)
        (var capacity_ref <ref i64>)
        (var size i64)) -> <ptr u8>
    (block
      (if (<= size 0)
        (block (return (cast 0 <ptr u8>))))

      ;; Little-endian bytes spell "QDBARENA".  It is metadata, not persisted
      ;; data, so only equality within the running kernel matters.
      (var magic = 4705774880233243729)
      (var count = count_ref)
      (var previous_capacity = 0)
      (if (> count 0)
        (block
          (var blocks = blocks_ref)
          (var chunk = (index blocks (- count 1)))
          (var header = (cast chunk <ptr i64>))
          ;; A foreign tail is an internal invariant violation.  Fail closed
          ;; before interpreting any of its bytes as arena metadata.
          (if (!= (index header 0) magic)
            (block (return (cast 0 <ptr u8>))))
          (var capacity = (index header 1))
          (var used = (index header 2))
          (if (! (&& (> capacity 0)
                     (>= used 0)
                     (<= used capacity)))
            (block (return (cast 0 <ptr u8>))))
          (= previous_capacity capacity)
          (if (<= size (- capacity used))
            (block
              (= header [2] (+ used size))
              (return (cast
                (+ (cast chunk i64) 24 used)
                <ptr u8>))))))

      (var chunk_capacity = 4096)
      (if (&& (> previous_capacity 0)
              (< previous_capacity 2097152))
        (block (= chunk_capacity (* previous_capacity 2))))
      (if (>= previous_capacity 2097152)
        (block (= chunk_capacity 2097152)))
      (if (< chunk_capacity size)
        (block (= chunk_capacity size)))
      (if (> chunk_capacity 9223372036854775783)
        (block (return (cast 0 <ptr u8>))))
      (if (! (call owned_arena_reserve_chunks
        blocks_ref count_ref capacity_ref 1))
        (block (return (cast 0 <ptr u8>))))

      (var chunk = (cast
        (call qdb_alloc (+ chunk_capacity 24)) <ptr u8>))
      (if (== (cast chunk i64) 0)
        (block (return (cast 0 <ptr u8>))))
      (var header = (cast chunk <ptr i64>))
      (= header [0] magic)
      (= header [1] chunk_capacity)
      (= header [2] size)
      (var blocks = blocks_ref)
      (var chunk_index = count_ref)
      (= blocks [chunk_index] chunk)
      (= count_ref (+ chunk_index 1))
      (return (cast (+ (cast chunk i64) 24) <ptr u8>))))

  ;; Keep HashTable ABI knowledge in these thin field-ref adapters.  Direct
  ;; tests cross the fifth chunk to exercise writing a reallocated registry
  ;; pointer back through the ref without mirroring the struct layout in C++.
  (fun aht_owned_arena_alloc
       ((var ht <ref HashTable>) (var size i64)) -> <ptr u8>
    (block
      (return (call owned_arena_alloc
        (field ht OwnedBlocks)
        (field ht OwnedBlockCount)
        (field ht OwnedBlockCapacity)
        size))))

  ;; rh_insert_stored cannot normally fail after capacity checks, but preserving
  ;; its failure contract requires rolling back the most recent arena slice.
  (fun owned_arena_rewind
       ((var blocks <ptr <ptr u8>>)
        (var count i64)
        (var block <ptr u8>)
        (var size i64))
    (block
      (if (<= size 0) (block (return)))
      (if (<= count 0) (block (return)))
      (var chunk = (index blocks (- count 1)))
      (var header = (cast chunk <ptr i64>))
      ;; Same little-endian "QDBARENA" tag as owned_arena_alloc above.
      (var magic = 4705774880233243729)
      (if (!= (index header 0) magic)
        (block (return)))
      (var capacity = (index header 1))
      (var used = (index header 2))
      (if (! (&& (> capacity 0)
                 (>= used size)
                 (<= used capacity)))
        (block (return)))
      (var expected =
        (+ (cast chunk i64) 24 (- used size)))
      (if (!= (cast block i64) expected) (block (return)))
      (= header [2] (- used size))))

  (fun aht_owned_arena_rewind
       ((var ht <ref HashTable>) (var block <ptr u8>) (var size i64))
    (block
      (call owned_arena_rewind
        (field ht OwnedBlocks)
        (field ht OwnedBlockCount)
        block size))))
