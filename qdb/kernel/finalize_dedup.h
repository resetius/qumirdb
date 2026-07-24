#pragma once

#include <qdb/kernel/generated.h>

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace NQdb {

std::string MakeKernelDedupKey(const TGeneratedKernel& kernel);

class TKernelDedupCache {
public:
    bool TryBind(
        TGeneratedKernel& kernel,
        const std::string& key,
        std::ostream* diagnostics);

    void Store(std::string key, const TGeneratedKernel& kernel);
    void PrintSummary(std::ostream* diagnostics) const;

private:
    struct TCompiledKernel {
        std::vector<void*> Fns;
        std::shared_ptr<NQumir::TLLVMRunner> Runner;
    };

    std::unordered_map<std::string, TCompiledKernel> Compiled_;
    size_t FinalizedCount_ = 0;
    size_t CacheHitCount_ = 0;
};

} // namespace NQdb
