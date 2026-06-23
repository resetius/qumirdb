#include <qdb/exec/aggregate_exec.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

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

struct TAggregateRowSetData {
    struct TAlignedByteBuffer {
        void Resize(size_t byteSize) {
            Words.resize((byteSize + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        }

        void* Data() {
            return Words.data();
        }

        std::vector<uint64_t> Words;
    };

    struct TKeyBuffer {
        TAlignedByteBuffer Fixed;
        std::vector<char> Data;
        std::vector<int64_t> Offsets;
        std::vector<uint8_t> Mask;
        TColumn Column{};
    };

    std::vector<TKeyBuffer> Keys;
    std::vector<std::vector<int64_t>> AggBuffers;
    std::vector<std::vector<uint8_t>> AggMasks;
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
    data->Keys.resize(Kernels_.OutputKeys.size());
    std::vector<int64_t> outputKeyBytes(Kernels_.OutputKeys.size());
    const int64_t measured = Kernels_.Measure(
        ht.data(), outputKeyBytes.data(), size);
    if (measured != size) {
        Kernels_.Dispatch(ht.data(), nullptr, 0, kOpDestroy);
        delete data;
        throw std::runtime_error("aggregate measure returned an unexpected row count");
    }
    std::vector<void*> outputKeyBuffers(Kernels_.OutputKeys.size());
    for (size_t i = 0; i < Kernels_.OutputKeys.size(); ++i) {
        auto& keyBuffer = data->Keys[i];
        if (Kernels_.OutputKeys[i].IsNullable) {
            keyBuffer.Mask.resize((size + 7) / 8);
        }
        auto* mask = keyBuffer.Mask.empty() ? nullptr : keyBuffer.Mask.data();
        if (Kernels_.OutputKeys[i].Kind == EAggregateOutputKeyKind::String) {
            keyBuffer.Data.resize(outputKeyBytes[i]);
            keyBuffer.Offsets.resize(size + 1);
            keyBuffer.Column = TColumn{
                .Data = keyBuffer.Data.data(),
                .Mask = mask,
                .Offsets = keyBuffer.Offsets.data(),
                .OffsetWidth = 8,
            };
            outputKeyBuffers[i] = &keyBuffer.Column;
        } else {
            keyBuffer.Fixed.Resize(outputKeyBytes[i]);
            keyBuffer.Column = TColumn{
                .Data = reinterpret_cast<char*>(keyBuffer.Fixed.Data()),
                .Mask = mask};
        }
        outputKeyBuffers[i] = &keyBuffer.Column;
    }
    data->AggBuffers.resize(Kernels_.NumAggs);
    data->AggMasks.resize(Kernels_.NumAggs);
    std::vector<int64_t*> outputBuffers(Kernels_.NumAggs);
    std::vector<uint8_t*> outputAggMasks(Kernels_.NumAggs, nullptr);
    for (size_t i = 0; i < Kernels_.NumAggs; ++i) {
        data->AggBuffers[i].resize(size);
        outputBuffers[i] = data->AggBuffers[i].data();
        if (i < Kernels_.OutputAggs.size() && Kernels_.OutputAggs[i].IsNullable) {
            data->AggMasks[i].resize((size + 7) / 8);
            outputAggMasks[i] = data->AggMasks[i].data();
        }
    }

    const int64_t finalized = Kernels_.Finalize(
        ht.data(), outputKeyBuffers.data(), outputBuffers.data(),
        outputAggMasks.data(), size);
    Kernels_.Dispatch(ht.data(), nullptr, 0, kOpDestroy);
    if (finalized != size) {
        delete data;
        throw std::runtime_error("aggregate finalize returned an unexpected row count");
    }

    data->Columns.reserve(Kernels_.OutputKeys.size() + Kernels_.NumAggs);
    for (auto& buffer : data->Keys) {
        data->Columns.push_back(buffer.Column);
    }
    for (size_t i = 0; i < data->AggBuffers.size(); ++i) {
        auto& mask = data->AggMasks[i];
        data->Columns.push_back(TColumn{
            .Data = reinterpret_cast<char*>(data->AggBuffers[i].data()),
            .Mask = mask.empty() ? nullptr : mask.data()});
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

} // namespace NQdb
