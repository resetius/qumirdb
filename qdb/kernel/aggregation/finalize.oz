(block
  (fun aggregation_finalize ((var ht <ref HashTable>)
                             (var output_keys <ptr i64>)
                             (var output_counts <ptr i64>)
                             (var output_sums <ptr i64>)
                             (var output_mins <ptr i64>)
                             (var output_maxs <ptr i64>)
                             (var output_capacity i64)) -> i64
    (block
      (var size = (field ht Size))
      (if (< output_capacity size)
        (block (return (: -1 i64))))
      (var group_keys = (field ht GroupKeys))
      (var agg_buffers = (field ht AggBuffers))
      (var counts = (index agg_buffers (: 0 i64)))
      (var sums = (index agg_buffers (: 1 i64)))
      (var mins = (index agg_buffers (: 2 i64)))
      (var maxs = (index agg_buffers (: 3 i64)))
      (var slot i64)
      (= slot (: 0 i64))
      (while (< slot size)
        (block
          (= output_keys [slot] (index group_keys slot))
          (= output_counts [slot] (index counts slot))
          (= output_sums [slot] (index sums slot))
          (= output_mins [slot] (index mins slot))
          (= output_maxs [slot] (index maxs slot))
          (= slot (+ slot (: 1 i64)))))
      (return size)))

  ;; NumAggs-generic finalize (L2b-2): output_buffers[0..NumAggs-1] mirror
  ;; AggBuffers[0..NumAggs-1] instead of 4 named output_counts/sums/mins/maxs.
  (fun agg_finalize ((var ht <ref HashTable>)
                     (var output_keys <ptr i64>)
                     (var output_buffers <ptr <ptr i64>>)
                     (var output_capacity i64)) -> i64
    (block
      (var size = (field ht Size))
      (if (< output_capacity size)
        (block (return (: -1 i64))))
      (var group_keys = (field ht GroupKeys))
      (var agg_buffers = (field ht AggBuffers))
      (var num_aggs = (field ht NumAggs))
      (var slot i64)
      (= slot (: 0 i64))
      (while (< slot size)
        (block
          (= output_keys [slot] (index group_keys slot))
          (= slot (+ slot (: 1 i64)))))
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
