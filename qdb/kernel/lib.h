#pragma once

#include <qumir/error.h>
#include <qumir/parser/ast.h>

#include <expected>
#include <string>
#include <unordered_set>
#include <vector>

namespace NQqb {
namespace NKernel {

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

} // namespace NKernel
} // namespace NQqb
