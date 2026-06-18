#pragma once

#include <qdb/kernel/join_key.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <string>

namespace NQqb::NKernel {

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
// left RowIds to pairs (right_row_id = -1, ignored by TJoinOutputBuilder when
// output schema contains only left columns).
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

// Generates the rh_hash / rh_key_equal overloads for the join key type, reusing
// the aggregation key-ops generator (GenKeyOperationFunDecls).
std::vector<NQumir::NAst::TExprPtr> GenJoinKeyOpsFunDecls(const TJoinKeyDescriptor& key);

// Emits the (type ...) declarations that make the named Key type(s) known to
// the compiler. Must be prepended to the program before any use of the Key.
std::vector<NQumir::NAst::TExprPtr> GenJoinKeyTypeDecls(const TJoinKeyDescriptor& key);

} // namespace NQqb::NKernel
