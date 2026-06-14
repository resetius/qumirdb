(block
  ;; Copies only aggregate states out of HashTable. The generated per-query
  ;; wrapper owns typed projection of dense GroupKeys into output columns.
  (fun aht_finalize_states
       ((var ht <ref HashTable>)
        (var output_buffers <ptr <ptr i64>>)
        (var num_aggs i64)
        (var output_capacity i64)) -> i64
    (block
      (var size = (field ht Size))
      (var result i64)
      (= result size)
      (if (< output_capacity size)
        (block (= result (: -1 i64))))
      (if (>= result (: 0 i64))
        (block
          (var agg_buffers = (field ht AggBuffers))
          (var a i64)
          (= a (: 0 i64))
          (while (< a num_aggs)
            (block
              (var src = (index agg_buffers a))
              (var dst = (index output_buffers a))
              (var slot i64)
              (= slot (: 0 i64))
              (while (< slot size)
                (block
                  (= dst [slot] (index src slot))
                  (= slot (+ slot (: 1 i64)))))
              (= a (+ a (: 1 i64)))))))
      (return result))))
