(block
  (pragma language overloads)

  ;; Default residual join filter: always passes. jt_emit_and_insert calls this
  ;; before pushing each matched pair. When a query has a residual predicate the
  ;; compiler replaces this definition with a generated one (gen.cpp,
  ;; GenJoinResidualFilterAst) that reads columns from the two row stores by
  ;; decoding the packed row IDs. The store / row-id parameters are unused here.
  (fun jt_residual_filter ((var left_store <ptr TRowSet>)
                           (var right_store <ptr TRowSet>)
                           (var left_row_id i64)
                           (var right_row_id i64)) -> bool
    (block (return #t))))
