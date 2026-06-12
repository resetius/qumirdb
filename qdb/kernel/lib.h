#pragma once

#include <qdb/kernel/aggregate_key.h>

#include <qumir/error.h>
#include <qumir/parser/ast.h>

#include <expected>
#include <string>
#include <unordered_set>
#include <vector>

namespace NQqb {
namespace NKernel {

// Reads an aggregation kernel source file (qdb/kernel/aggregation/<name>),
// resolved relative to this source file's location so it works regardless
// of the caller's current working directory.
std::string ReadAggregationKernel(const std::string& name);

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

// Builds (block <library...> <entry>), with `entry` last so that
// TLLVMRunner::CompileKernelAst (which returns Module.Functions.back())
// picks it as the compiled kernel's entry point.
NQumir::NAst::TExprPtr MergeKernelLibrary(
    std::vector<NQumir::NAst::TExprPtr> library,
    NQumir::NAst::TExprPtr entry);

// Composes one generic aggregation update program in dependency order:
// key operations, reducers, generic rehash/table libraries, then the named
// dispatch entry. The returned AST is unresolved and must be compiled with
// AllowOverloads=true and explicit entry name "agg_dispatch".
std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateProgramAst(
    const NQumir::NAst::TStructType& inputType,
    const TAggregateKeyDescriptor& key,
    const std::optional<std::string>& argField,
    const std::vector<std::string>& reducers,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType,
    NQumir::NAst::TTypePtr hashTableType);

std::expected<NQumir::NAst::TExprPtr, NQumir::TError>
BuildGenericAggregateFinalizeProgramAst(
    const TAggregateKeyDescriptor& key,
    NQumir::NAst::TTypePtr hashTableType);

} // namespace NKernel
} // namespace NQqb
