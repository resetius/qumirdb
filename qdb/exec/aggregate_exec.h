#pragma once

#include <qdb/exec/unary_block_exec.h>
#include <qdb/kernel/compiler.h>

#include <array>
#include <cstdint>

namespace NQdb {

class TAggregateProcessor {
public:
    explicit TAggregateProcessor(TAggregateKernels kernels);
    ~TAggregateProcessor();

    TAggregateProcessor(const TAggregateProcessor&) = delete;
    TAggregateProcessor& operator=(const TAggregateProcessor&) = delete;

    void Add(TRowSet& rowSet);
    bool Finish(TRowSet& rowSet);

private:
    void Destroy();

private:
    TAggregateKernels Kernels_;
    std::array<uint8_t, TKernelCompiler::kHashTableSize> HashTable_ = {};
    bool Finished_ = false;
    bool Destroyed_ = false;
};

TRuntimeUnaryBlockingKernel::TProcess MakeAggregateProcess(
    TAggregateKernels kernels);

} // namespace NQdb
