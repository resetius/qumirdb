#pragma once

#include <qdb/kernel/aggregate_key.h>
#include <qdb/kernel/gen.h>

#include <qumir/error.h>
#include <qumir/parser/ast.h>

#include <expected>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NQdb {
namespace NKernel {

// Reads an aggregation kernel source file (qdb/kernel/aggregation/<name>),
// resolved relative to this source file's location so it works regardless
// of the caller's current working directory.
std::string ReadAggregationKernel(const std::string& name);

// Reads a join kernel source file (qdb/kernel/join/<name>), resolved relative
// to this source file's location.
std::string ReadJoinKernel(const std::string& name);

// Reads a sort kernel source file (qdb/kernel/sort/<name>), resolved relative
// to this source file's location.
std::string ReadSortKernel(const std::string& name);

// Assembles the join kernel library: generic Robin Hood helpers, the reused
// aggregation HashTable lifecycle, pair buffers, and join update helpers.
// Per-query key readers and the external jt_dispatch entry are generated in
// join_gen.cpp and appended by TKernelCompiler::CompileJoin.
std::expected<std::vector<NQumir::NAst::TExprPtr>, NQumir::TError>
BuildJoinKernelLibrary();

// Parses `source` as a top-level (block (fun ...) ...) and returns its
// FunDecl statements, in order, skipping any whose Name is in `exclude`.
// The result is unresolved/unannotated, like GenFilterKernelAst's output:
// callers merge it with a generated entry point via MergeKernelLibrary and
// pass the combined block to TLLVMRunner::CompileKernelAst, which resolves
// and lowers everything together.
std::expected<std::vector<NQumir::NAst::TExprPtr>, NQumir::TError>
ParseFunctionLibrary(
    const std::string& source,
    const std::unordered_set<std::string>& exclude = {});

// Builds (block <library...> <entry>). Callers select the compiled entry point
// explicitly by its source function name.
NQumir::NAst::TExprPtr MergeKernelLibrary(
    std::vector<NQumir::NAst::TExprPtr> library,
    NQumir::NAst::TExprPtr entry);

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildFilterProgramAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr stringViewType,
    std::vector<std::shared_ptr<std::string>>& literalStorage);

// Composes one generic aggregation update program in dependency order:
// key operations, reducers, generic rehash/table libraries, then the named
// dispatch entry. The returned AST is unresolved and must be compiled with
// AllowOverloads=true and explicit entry name "agg_dispatch".
std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateProgramAst(
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType);

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateFinalizeProgramAst(
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr hashTableType,
    NQumir::NAst::TTypePtr columnType = nullptr);

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateMeasureProgramAst(
    const TAggregateKeyDescriptor& key,
    NQumir::NAst::TTypePtr hashTableType);

// One program with aggregate update/finalize entries sharing a single copy of
// the type decls and libraries. Internal measure/finalize helpers stay in the
// module; external callers use agg_dispatch and agg_finish_rowset.
std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateFusedProgramAst(
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const TAggReducerLayout& layout,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType);

} // namespace NKernel
} // namespace NQdb
