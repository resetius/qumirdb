(block
  ;; Cross join emits canonical (left_id, right_id) pairs into PairBuffer.
  ;; Materialization is shared with equi-join through jt_materialize.

  (fun xj_selected ((var batch <ref TRowSet>) (var row i64)) -> bool
    (block
      (var selection = (field batch Selection))
      (if (== (cast selection i64) (: 0 i64))
        (block (return #t)))
      (return (!= (index selection row) (: 0 u8)))))

  (fun xj_row_id ((var batch_idx i64) (var row i64)) -> i64
    (block
      (return (+ (<< batch_idx (: 32 i64)) (& row (: 4294967295 i64))))))

  (fun xj_emit ((var batch <ref TRowSet>)
                (var batch_idx i64)
                (var right_store <ptr TRowSet>)
                (var right_batch_count i64)
                (var pairs <ref PairBuffer>)) -> bool
    (block
      (var l i64)
      (= l (: 0 i64))
      (while (< l (field batch RowCount))
        (block
          (if (call xj_selected batch l)
            (block
              (var left_id = (call xj_row_id batch_idx l))
              (var rb i64)
              (= rb (: 0 i64))
              (while (< rb right_batch_count)
                (block
                  (var right_batch = (index right_store rb))
                  (var r i64)
                  (= r (: 0 i64))
                  (while (< r (field right_batch RowCount))
                    (block
                      (if (call xj_selected right_batch r)
                        (block
                          (if (! (call pb_push pairs left_id (call xj_row_id rb r)))
                            (block (return #f)))))
                      (= r (+ r (: 1 i64)))))
                  (= rb (+ rb (: 1 i64)))))))
          (= l (+ l (: 1 i64)))))
      (return #t)))

  ;; xj_dispatch(batch, batch_idx, right_store, right_batch_count, pairs, op)
  ;;   op == 0: emit pairs for one left batch against the buffered right store
  ;;   op == 1: destroy PairBuffer storage
  (fun xj_dispatch ((var batch <ref TRowSet>)
                    (var batch_idx i64)
                    (var right_store <ptr TRowSet>)
                    (var right_batch_count i64)
                    (var pairs <ref PairBuffer>)
                    (var op i64)) -> bool
    (block
      (if (== op (: 0 i64))
        (block
          (return (call xj_emit batch batch_idx right_store right_batch_count pairs))))
      (if (== op (: 1 i64))
        (block
          (call pb_destroy pairs)
          (return #t)))
      (return #f))))
