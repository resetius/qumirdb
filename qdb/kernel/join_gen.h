#pragma once

#include <qdb/kernel/join_key.h>
#include <qdb/plan/ops/join.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <string>

namespace NQdb::NKernel {

// Generates jt_insert_key_only: iterates a batch, extracts the key, and
// inserts only the key into the own table (no row-ID storage via jb_append).
// Used for the right side of SEMI/ANTI joins. Signature is identical to
// GenJoinProcessAst so the same TProcessFn ABI can be reused; `opp` and
// `pairs` parameters are present but unused in the generated body.
NQumir::NAst::TExprPtr GenJoinInsertKeyOnlyAst(
    const TJoinKeyDescriptor& key,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType);

// Generates jt_finalize_semi / jt_finalize_anti: iterates own.GroupKeys,
// probes opp for each key, and pushes matching (SEMI) or non-matching (ANTI)
// left RowIds to pairs (right_row_id = -1; jt_materialize ignores it because
// semi/anti output schema contains only left columns).
NQumir::NAst::TExprPtr GenJoinFinalizeSemiAntiAst(
    const TJoinKeyDescriptor& key,
    bool isAnti,
    const std::string& funcName,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType);

// Generates one side's process function (jt_process_left / jt_process_right):
// reads the side's key columns, assembles the <named Key>, and calls the
// generic jt_emit_and_insert for each selected row. Reuses BuildColumnValueAst
// for the per-row key materialization (same path as the aggregation dispatch).
//
// Signature of the generated function:
//   <funcName>(own: <ref HashTable>, opp: <ref HashTable>,
//              batch: <ref TRowSet>, batch_idx: i64, pairs: <ref PairBuffer>) -> bool
//
// Stage-4 scope: fixed-width keys only (integer/f64/composite). Throws
// NQumir::TError for string keys (which need the dual-key path).
NQumir::NAst::TExprPtr GenJoinProcessAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType);

// Generates one side's probe-only function (jt_probe_left_stream /
// jt_probe_right_stream): reads stream batch keys, probes the already-built
// opposite table, and emits pairs without inserting stream rows into any table.
NQumir::NAst::TExprPtr GenJoinProbeAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType);

// Generates one side's rowset hash helper for shuffle:
//   <funcName>(batch: <ref TRowSet>, hashes: <ptr u64>) -> bool
// Fills hashes[i] for every physical row i in the batch. Selection is not
// applied here; shuffle/scatter code must skip unselected rows itself.
NQumir::NAst::TExprPtr GenJoinHashBatchAst(
    const TJoinKeyDescriptor& key,
    bool isLeft,
    const std::string& funcName,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType);

// Generates the rh_hash / rh_key_equal overloads for the join key type, reusing
// the aggregation key-ops generator (GenKeyOperationFunDecls).
std::vector<NQumir::NAst::TExprPtr> GenJoinKeyOpsFunDecls(const TJoinKeyDescriptor& key);

// Emits the (type ...) declarations that make the named Key type(s) known to
// the compiler. Must be prepended to the program before any use of the Key.
std::vector<NQumir::NAst::TExprPtr> GenJoinKeyTypeDecls(const TJoinKeyDescriptor& key);

// Generates the output materializer entrypoint:
//   jt_materialize(pairs, left_store, right_store, stream_left, stream_right,
//                  start, limit, out) -> i64
// Gathers up to `limit` pairs beginning at `start` from the pair buffer into a
// complete output TRowSet: allocates every buffer with qdb_alloc (recorded in
// an owners list stored in out.Private), decodes packed row ids (id == -1 is
// outer-join null padding; batch index -1 reads the stream_* batch instead of
// the store), and writes fixed-width values, string payloads (two passes:
// sizes into Offsets, then bytes), and validity masks. Output columns are the
// left fields followed by the right fields (right omitted for semi/anti); the
// per-column read/write code is fully specialized at generation time. Returns
// the number of rows materialized (0 leaves `out` untouched). The caller
// assigns out.Destroy (the kernel sets it to null).
NQumir::NAst::TExprPtr GenJoinMaterializeAst(
    const NQumir::NAst::TStructType& leftType,
    const NQumir::NAst::TStructType& rightType,
    bool includeRight,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr pairBufferType,
    NQumir::NAst::TTypePtr stringViewType);

// Generates the single external join entrypoint:
//   jt_dispatch(left, right, batch, batch_idx, pairs, left_store, right_store,
//               arg, op) -> bool
// It dispatches init/update/stream/finalize/destroy to the internal generated
// and library helpers. Join type and key size are compile-time constants.
NQumir::NAst::TExprPtr GenJoinDispatchAst(
    int64_t keySize,
    EJoinType type,
    bool hasResidual,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr pairBufferType);

} // namespace NQdb::NKernel
