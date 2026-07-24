#include <qdb/kernel/finalize_dedup.h>

#include <qumir/parser/core/printer.h>

#include <ostream>
#include <string>
#include <utility>

namespace NQdb {

std::string MakeKernelDedupKey(const TGeneratedKernel& kernel) {
    std::string key = NQumir::NAst::NCore::PrintAst(kernel.Ast);
    key += "\n# qdb-kernel-entrypoints\n";
    for (const auto& entrypoint : kernel.Entrypoints) {
        key += std::to_string(entrypoint.size());
        key += ':';
        key += entrypoint;
        key += '\n';
    }
    return key;
}

bool TKernelDedupCache::TryBind(
    TGeneratedKernel& kernel,
    const std::string& key,
    std::ostream* diagnostics)
{
    ++FinalizedCount_;

    auto cached = Compiled_.find(key);
    if (cached == Compiled_.end()) {
        return false;
    }

    kernel.Slot->Fns = cached->second.Fns;
    kernel.Slot->Runner = cached->second.Runner;
    ++CacheHitCount_;

    if (diagnostics) {
        *diagnostics << "\n========== RUNTIME FINALIZE: " << kernel.Name
                     << " (" << kernel.Stage << ") ==========\n"
                     << "[kernel-dedup] cache hit\n"
                     << "========== END RUNTIME FINALIZE ==========\n";
    }
    return true;
}

void TKernelDedupCache::Store(std::string key, const TGeneratedKernel& kernel) {
    Compiled_.emplace(
        std::move(key),
        TCompiledKernel{
            .Fns = kernel.Slot->Fns,
            .Runner = kernel.Slot->Runner,
        });
}

void TKernelDedupCache::PrintSummary(std::ostream* diagnostics) const {
    if (!diagnostics || FinalizedCount_ == 0) {
        return;
    }

    *diagnostics << "[kernel-dedup] unique kernels: "
                 << Compiled_.size()
                 << " / "
                 << FinalizedCount_
                 << ", hits: "
                 << CacheHitCount_
                 << "\n";
}

} // namespace NQdb
