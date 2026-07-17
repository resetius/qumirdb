(block
  (pragma language overloads)

  (fun top_sort_temp_before_state [Key]
       ((var state_key Key)
        (var temp_key Key)
        (var desc bool)) -> bool
    (block
      (if desc
        (block
          (return (> temp_key state_key)))
        (block
          (return (< temp_key state_key))))))

  (fun top_sort_merge_picks [Key]
       ((var state_keys <ptr Key>)
        (var state_n i64)
        (var temp_keys <ptr Key>)
        (var temp_indices <ptr u32>)
        (var temp_n i64)
        (var pick_src <ptr u8>)
        (var pick_idx <ptr u32>)
        (var limit i64)
        (var desc bool)) -> i64
    (block
      (var left = (: 0 i64))
      (var right = (: 0 i64))
      (var out = (: 0 i64))
      (while (&& (< out limit) (|| (< left state_n) (< right temp_n)))
        (block
          (if (== right temp_n)
            (block
              (= pick_src [out] (: 0 u8))
              (= pick_idx [out] (cast left u32))
              (= left (+ left (: 1 i64))))
            (block
              (if (== left state_n)
                (block
                  (= pick_src [out] (: 1 u8))
                  (= pick_idx [out] (index temp_indices right))
                  (= right (+ right (: 1 i64))))
                (block
                  (var temp_index = (index temp_indices right))
                  (if (call top_sort_temp_before_state
                        (index state_keys left)
                        (index temp_keys (cast temp_index i64))
                        desc)
                    (block
                      (= pick_src [out] (: 1 u8))
                      (= pick_idx [out] temp_index)
                      (= right (+ right (: 1 i64))))
                    (block
                      (= pick_src [out] (: 0 u8))
                      (= pick_idx [out] (cast left u32))
                      (= left (+ left (: 1 i64)))))))))
          (= out (+ out (: 1 i64)))))
      (return out)))

  (fun top_sort_gather_column [Value]
       ((var state_values <ptr Value>)
        (var temp_values <ptr Value>)
        (var pick_src <ptr u8>)
        (var pick_idx <ptr u32>)
        (var out_values <ptr Value>)
        (var n i64))
    (block
      (var i = (: 0 i64))
      (while (< i n)
        (block
          (var idx = (cast (index pick_idx i) i64))
          (if (== (index pick_src i) (: 0 u8))
            (block
              (= out_values [i] (index state_values idx)))
            (block
              (= out_values [i] (index temp_values idx))))
          (= i (+ i (: 1 i64)))))))
)
