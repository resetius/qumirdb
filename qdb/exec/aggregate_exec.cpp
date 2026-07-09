#include <qdb/exec/aggregate_exec.h>
#include <qdb/exec/kernel_rowset.h>
#include <qdb/modules/qumirdb_runtime.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace NQdb {

namespace {

// Mirrors the HashTable C layout from qdb/modules/qumirdb.cpp.
struct THashTable {
    uint8_t* Keys = nullptr;
    int64_t* Dist = nullptr;
    int64_t* SlotId = nullptr;
    uint8_t* GroupKeys = nullptr;
    int64_t** AggBuffers = nullptr;
    uint8_t** OwnedBlocks = nullptr;
    int64_t OwnedBlockCount = 0;
    int64_t OwnedBlockCapacity = 0;
    int64_t Capacity = 0;
    int64_t Size = 0;
    int64_t NumAggs = 0;
    int64_t NumKeys = 0;
    int64_t KeySize = 0;
};
static_assert(sizeof(THashTable) == TKernelCompiler::kHashTableSize);

constexpr int64_t kInitialCapacity = 4;
constexpr int64_t kOpInit = 0;
constexpr int64_t kOpUpdate = 1;
constexpr int64_t kOpDestroy = 2;

} // namespace

TAggregateProcessor::TAggregateProcessor(TAggregateKernels kernels)
    : Kernels_(std::move(kernels))
{
}

TAggregateProcessor::~TAggregateProcessor()
{
    Destroy();
}

// Deferred so a processor can be constructed before its kernels are finalized.
void TAggregateProcessor::EnsureInit()
{
    if (Initialized_) {
        return;
    }
    Initialized_ = true;
    Kernels_.Dispatch(HashTable_.data(), nullptr, kInitialCapacity, kOpInit);
}

void TAggregateProcessor::Add(TRowSet& rowSet)
{
    if (Finished_) {
        throw std::runtime_error("aggregate processor is already finished");
    }
    EnsureInit();
    Kernels_.Dispatch(HashTable_.data(), &rowSet, 0, kOpUpdate);
}

bool TAggregateProcessor::Finish(TRowSet& rowSet)
{
    if (Finished_) {
        return false;
    }
    Finished_ = true;
    EnsureInit();

    const int64_t size = reinterpret_cast<THashTable*>(HashTable_.data())->Size;
    const int64_t finalized = Kernels_.FinishRowSet(HashTable_.data(), &rowSet);
    Destroyed_ = true;
    if (finalized != size) {
        throw std::runtime_error("aggregate finalize returned an unexpected row count");
    }

    rowSet.Destroy = DestroyKernelOwnedRowSet;
    return true;
}

void TAggregateProcessor::Destroy()
{
    if (Initialized_ && !Destroyed_) {
        Kernels_.Dispatch(HashTable_.data(), nullptr, 0, kOpDestroy);
        Destroyed_ = true;
    }
}

} // namespace NQdb
