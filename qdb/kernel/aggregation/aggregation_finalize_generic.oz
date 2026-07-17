(block
  (pragma language overloads)

  ;; Copies dense Key values and i64 aggregate states out of HashTable.
  ;; The generated per-query wrapper supplies the concrete output Key pointer;
  ;; future composite wrappers project Key fields into separate output columns.
  (fun aht_finalize [Key]
       ((var ht <ref HashTable>)
        (var output_keys <ptr Key>)
        (var output_buffers <ptr <ptr i64>>)
        (var output_capacity i64)) -> i64
    (block
      (var size = (field ht Size))
      (if (< output_capacity size)
        (block (return (: -1 i64))))
      (var group_keys =
        (cast (field ht GroupKeys)
              <ptr Key>))
      (var slot i64)
      (= slot (: 0 i64))
      (while (< slot size)
        (block
          (= output_keys [slot] (index group_keys slot))
          (= slot (+ slot (: 1 i64)))))
      (var agg_buffers = (field ht AggBuffers))
      (var num_aggs = (field ht NumAggs))
      (var a i64)
      (= a (: 0 i64))
      (while (< a num_aggs)
        (block
          (var src = (index agg_buffers a))
          (var dst = (index output_buffers a))
          (= slot (: 0 i64))
          (while (< slot size)
            (block
              (= dst [slot] (index src slot))
              (= slot (+ slot (: 1 i64)))))
          (= a (+ a (: 1 i64)))))
      (return size))))
