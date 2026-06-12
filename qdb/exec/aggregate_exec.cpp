#include <qdb/exec/aggregate_exec.h>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace NQqb {

namespace {

// Mirrors the HashTable C layout from qdb/modules/qumirdb.cpp (96 bytes).
struct THashTable {
    int64_t* Keys = nullptr;
    int64_t* Dist = nullptr;
    int64_t* SlotId = nullptr;
    int64_t* GroupKeys = nullptr;
    int64_t** AggBuffers = nullptr;
    int64_t* Scratch = nullptr;
    int64_t* Scratch2 = nullptr;
    int64_t* QueryKey = nullptr;
    int64_t Capacity = 0;
    int64_t Size = 0;
    int64_t NumAggs = 0;
    int64_t NumKeys = 0;
};
static_assert(sizeof(THashTable) == TKernelCompiler::kHashTableSize);

constexpr int64_t kInitialCapacity = 4;
constexpr int64_t kOpInit = 0;
constexpr int64_t kOpUpdate = 1;
constexpr int64_t kOpDestroy = 2;

struct TAggregateRowSetData {
    std::vector<int64_t> Keys;
    std::vector<std::vector<int64_t>> AggBuffers;
    std::vector<TColumn> Columns;
};

void DestroyAggregateRowSet(TRowSet* rowSet) {
    delete static_cast<TAggregateRowSetData*>(rowSet->Private);
}

} // namespace

TRuntimeAggregate::TRuntimeAggregate(
    std::unique_ptr<IRuntimeNode> input,
    NQumir::NAst::TTypePtr outputType,
    TAggregateKernels kernels)
    : Input_(std::move(input))
    , OutputType_(std::move(outputType))
    , Kernels_(std::move(kernels))
{}

bool TRuntimeAggregate::Next(TRowSet& rowSet) {
    if (Done_) {
        return false;
    }
    Done_ = true;

    std::array<uint8_t, TKernelCompiler::kHashTableSize> ht{};
    Kernels_.Dispatch(ht.data(), nullptr, kInitialCapacity, kOpInit);

    TRowSet batch = {};
    while (Input_->Next(batch)) {
        Kernels_.Dispatch(ht.data(), &batch, 0, kOpUpdate);
        Release(&batch);
    }

    const int64_t size = reinterpret_cast<THashTable*>(ht.data())->Size;

    auto* data = new TAggregateRowSetData;
    data->Keys.resize(size);
    data->AggBuffers.resize(Kernels_.NumAggs);
    std::vector<int64_t*> outputBuffers(Kernels_.NumAggs);
    for (size_t i = 0; i < Kernels_.NumAggs; ++i) {
        data->AggBuffers[i].resize(size);
        outputBuffers[i] = data->AggBuffers[i].data();
    }

    Kernels_.Finalize(ht.data(), data->Keys.data(), outputBuffers.data(), size);
    Kernels_.Dispatch(ht.data(), nullptr, 0, kOpDestroy);

    data->Columns.reserve(1 + Kernels_.NumAggs);
    data->Columns.push_back(TColumn{.Data = reinterpret_cast<char*>(data->Keys.data())});
    for (auto& buffer : data->AggBuffers) {
        data->Columns.push_back(TColumn{.Data = reinterpret_cast<char*>(buffer.data())});
    }

    rowSet = {
        .Columns = data->Columns.data(),
        .ColumnCount = static_cast<int64_t>(data->Columns.size()),
        .RowCount = size,
        .Selection = nullptr,
        .Destroy = DestroyAggregateRowSet,
        .Private = data,
        .RefCount = 1,
    };
    return true;
}

} // namespace NQqb
