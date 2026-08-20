(block
  ;; SwissTable group primitives: absl::container_internal::GroupPortableImpl
  ;; over 8-slot groups. A control byte is 0x80 when the slot is empty, and
  ;; H2 (hash & 0x7F) when it is full; deletion is not supported, so there are
  ;; no tombstones and no sentinel.
  ;;
  ;; lsbs = 0x0101010101010101; msbs = lsbs << 7 = 0x8080808080808080, which is
  ;; written as a shift because it does not fit a signed 64-bit literal.

  ;; Bits 8k+7 of the result mark slots whose control byte may equal h2.
  ;; Like absl, this can report a false positive, but never a false negative
  ;; and never on an empty byte, so the caller's key comparison settles it.
  ;; (~x & msbs is written as msbs ^ (x & msbs): oz has no bitwise not.)
  (fun swiss_match ((var word u64) (var h2 u64)) -> u64
    (block
      (var lsbs = (: 72340172838076673 u64))
      (var msbs = (<< lsbs (: 7 u64)))
      (var x = (^ word (* lsbs h2)))
      (return (& (- x lsbs) (^ msbs (& x msbs))))))

  ;; Empty slots are exactly the bytes with the high bit set.
  (fun swiss_match_empty ((var word u64)) -> u64
    (block
      (var msbs = (<< (: 72340172838076673 u64) (: 7 u64)))
      (return (& word msbs))))

  ;; Slot index within the group of the lowest set bit. Mask bits only ever
  ;; sit at 8k+7, so the trailing-zero count divided by 8 is the index.
  (fun swiss_lowest_index ((var mask u64)) -> i64
    (block
      (return (>> (call builtin::cttz mask) (: 3 i64))))))
