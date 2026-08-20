(block
  (pragma language overloads)

  ;; Rebuilds only the probe table. Dense group-key and aggregate buffers are
  ;; indexed by slot_id and therefore do not move here.
  (fun rh_rehash_into [Key] ((var old_keys <ptr Key>)
                       (var old_ctrl <ptr u8>)
                       (var old_slot_ids <ptr i64>)
                       (var old_capacity i64)
                       (var new_keys <ptr Key>)
                       (var new_ctrl <ptr u8>)
                       (var new_slot_ids <ptr i64>)
                       (var new_capacity i64)) -> bool
    (block
      (var index i64)
      (= index (: 0 i64))
      (while (< index new_capacity)
        (block
          (= new_ctrl [index] (: 128 u8))
          (= new_slot_ids [index] (: -1 i64))
          (= index (+ index (: 1 i64)))))

      (= index (: 0 i64))
      (while (< index old_capacity)
        (block
          (if (< (index old_ctrl index) (: 128 u8))
            (block
              (if (! (call rh_insert_displace
                new_keys new_ctrl new_slot_ids new_capacity
                (index old_keys index) (index old_slot_ids index)))
                (block (return #f)))))
          (= index (+ index (: 1 i64)))))
      (return #t))))
