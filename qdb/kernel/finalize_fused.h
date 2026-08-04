#pragma once

#include <qdb/kernel/generated.h>

#include <qumir/parser/ast.h>

#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace NQdb {

// Dedup key for a whole kernel: identical AST + entrypoints compile once.
std::string MakeKernelDedupKey(const TGeneratedKernel& kernel);

struct TFusedProgram {
    NQumir::NAst::TExprPtr Program;
    std::vector<std::string> Entrypoints;                    // deduped, for the compiler
    std::vector<std::vector<std::string>> UniqueEntrypoints; // per input kernel, fused names
    // Per input kernel: renamed function names (original -> fused). Only entries
    // that changed; callers apply `renames[name]` falling back to `name`.
    std::vector<std::unordered_map<std::string, std::string>> UniqueRenames;
    size_t TypeDeclCount = 0;
    size_t FunctionDeclCount = 0;
};

// Merges kernels into one program: identical decls shared, conflicting
// query-private decls prefixed __qdb_k<i>_ (rename propagated to callers).
// Reusable helpers with identical bodies keep their canonical names.
TFusedProgram BuildFusedProgram(std::span<TGeneratedKernel* const> uniqueKernels);

} // namespace NQdb
