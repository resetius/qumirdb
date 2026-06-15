(block
  (pragma language overloads)

  ;; Symmetric hash join probe+insert, generic over the key type.
  ;; The dense bucket lives in the reused aggregation HashTable's AggBuffers
  ;; (NumAggs=3: length / capacity / data-ptr-as-i64, see join_table.oz). Key
  ;; reading/assembly is generated per-query (join_gen.cpp); this generic core
  ;; takes the already-assembled <named Key> and reuses rh_lookup_slot /
  ;; rh_insert_displace / aht_rehash from aggregation.

  ;; Probe the opposite table for `key`, emit one pair per matching stored row,
  ;; then insert `own_row_id` under `key` into the own table. is_left orders the
  ;; emitted pair as (left, right). Returns #f only on allocation failure.
  (fun jt_emit_and_insert ((var own <ref HashTable>)
                           (var opp <ref HashTable>)
                           (var key <named Key (template readable mutable)>)
                           (var own_row_id i64)
                           (var is_left i64)
                           (var pairs <ref PairBuffer>)) -> bool
    (block
      (var opp_keys =
        (cast (field opp Keys) <ptr <named Key (template readable mutable)>>))
      (var opp_slot = (call rh_lookup_slot opp_keys (field opp Dist)
                        (field opp SlotId) (field opp Capacity) key))
      (if (!= opp_slot (: -1 i64))
        (block
          (var opp_aggs = (field opp AggBuffers))
          (var opp_counts = (index opp_aggs (: 0 i64)))
          (var opp_datas = (index opp_aggs (: 2 i64)))
          (var bcount = (index opp_counts opp_slot))
          (var bdata = (cast (index opp_datas opp_slot) <ptr i64>))
          (var k i64)
          (= k (: 0 i64))
          (while (< k bcount)
            (block
              (var opp_row_id = (index bdata k))
              (if (!= is_left (: 0 i64))
                (block
                  (if (! (call pb_push pairs own_row_id opp_row_id))
                    (block (return #f))))
                (block
                  (if (! (call pb_push pairs opp_row_id own_row_id))
                    (block (return #f)))))
              (= k (+ k (: 1 i64)))))))
      (var own_keys =
        (cast (field own Keys) <ptr <named Key (template readable mutable)>>))
      (var own_slot = (call rh_lookup_slot own_keys (field own Dist)
                        (field own SlotId) (field own Capacity) key))
      (if (== own_slot (: -1 i64))
        (block
          (var capacity = (field own Capacity))
          (var size = (field own Size))
          (if (> (+ size (: 1 i64)) (- capacity (/ capacity (: 4 i64))))
            (block
              (if (! (call aht_rehash own (* capacity (: 2 i64)) key))
                (block (return #f)))
              (= capacity (field own Capacity))))
          (= own_keys
            (cast (field own Keys) <ptr <named Key (template readable mutable)>>))
          (= own_slot size)
          (if (! (call rh_insert_displace own_keys (field own Dist)
                        (field own SlotId) capacity key own_slot))
            (block (return #f)))
          (var group_keys =
            (cast (field own GroupKeys) <ptr <named Key (template readable mutable)>>))
          (= group_keys [own_slot] key)
          (var own_aggs = (field own AggBuffers))
          (var own_counts = (index own_aggs (: 0 i64)))
          (var own_caps = (index own_aggs (: 1 i64)))
          (var own_datas = (index own_aggs (: 2 i64)))
          (= own_counts [own_slot] (: 0 i64))
          (= own_caps [own_slot] (: 0 i64))
          (= own_datas [own_slot] (: 0 i64))
          (field_assign own Size (+ size (: 1 i64)))))
      (if (! (call jb_append own own_slot own_row_id))
        (block (return #f)))
      (return #t)))

  ;; Concrete i64 reader (Stage 1 entry): reads an i64 key column directly and
  ;; delegates per row to jt_emit_and_insert<i64>. Generic-key queries use the
  ;; generated jt_process_left / jt_process_right instead (join_gen.cpp).
  (fun jt_process_batch ((var own <ref HashTable>)
                         (var opp <ref HashTable>)
                         (var batch <ref TRowSet>)
                         (var key_col_idx i64)
                         (var batch_idx i64)
                         (var is_left i64)
                         (var pairs <ref PairBuffer>)) -> bool
    (block
      (var columns = (field batch Columns))
      (var row_count = (field batch RowCount))
      (var selection = (field batch Selection))
      (var key_col = (index columns key_col_idx))
      (var key_data = (cast (field key_col Data) <ptr i64>))
      (var row i64)
      (= row (: 0 i64))
      (while (< row row_count)
        (block
          (var selected bool)
          (= selected #t)
          (if (!= (cast selection i64) (: 0 i64))
            (block
              (if (== (index selection row) (: 0 u8))
                (block (= selected #f)))))
          (if selected
            (block
              (var key = (index key_data row))
              (var own_row_id = (+ (<< batch_idx (: 32 i64))
                                   (& row (: 4294967295 i64))))
              (if (! (call jt_emit_and_insert own opp key own_row_id is_left pairs))
                (block (return #f)))))
          (= row (+ row (: 1 i64)))))
      (return #t))))
