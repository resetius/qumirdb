#include <qdb/scheduler/connection.h>

#include <qdb/scheduler/spsc.h>

#include <atomic>
#include <cassert>
#include <utility>

namespace NQdb {
namespace NScheduler {
namespace {

TRowSet TakeRowSet(TRowSet& rowSet) {
    auto out = rowSet;
    rowSet = {};
    return out;
}

} // namespace

struct TOneToOneConnection::TLane {
    explicit TLane(size_t capacity)
        : Queue(capacity)
    {}

    ~TLane() {
        TRowSet rowSet{};
        while (Queue.TryPop(rowSet)) {
            Release(&rowSet);
        }
    }

    TSPSC<TRowSet> Queue;
    std::atomic<bool> Finished = false;
};

TOneToOneConnection::TOneToOneConnection(size_t capacity)
    : Capacity_(capacity)
{
    Resize(1, 1);
}

TOneToOneConnection::~TOneToOneConnection() = default;

EConnectionKind TOneToOneConnection::Kind() const {
    return EConnectionKind::OneToOne;
}

void TOneToOneConnection::Resize(size_t srcCount, size_t dstCount) {
    assert(srcCount == dstCount);
    Size_ = srcCount;
    Lanes_.clear();
    Lanes_.reserve(Size_);
    for (size_t i = 0; i < Size_; ++i) {
        Lanes_.push_back(std::make_unique<TLane>(Capacity_));
    }
}

size_t TOneToOneConnection::SrcCount() const {
    return Size_;
}

size_t TOneToOneConnection::DstCount() const {
    return Size_;
}

bool TOneToOneConnection::CanPush(size_t srcId) const {
    assert(srcId < Size_);
    return Lanes_[srcId]->Queue.CanPush();
}

bool TOneToOneConnection::Push(size_t srcId, TRowSet&& rowSet) {
    assert(srcId < Size_);
    auto moved = TakeRowSet(rowSet);
    if (Lanes_[srcId]->Queue.TryPush(std::move(moved))) {
        return true;
    }
    rowSet = TakeRowSet(moved);
    return false;
}

void TOneToOneConnection::Finish(size_t srcId) {
    assert(srcId < Size_);
    Lanes_[srcId]->Finished.store(true, std::memory_order_release);
}

EFetchResult TOneToOneConnection::Fetch(size_t dstId, TRowSet& rowSet) {
    assert(dstId < Size_);
    if (Lanes_[dstId]->Queue.TryPop(rowSet)) {
        return EFetchResult::OK;
    }

    return Lanes_[dstId]->Finished.load(std::memory_order_acquire)
        ? EFetchResult::FINISHED
        : EFetchResult::NO_DATA;
}

struct TGatherConnection::TLane {
    explicit TLane(size_t capacity)
        : Queue(capacity)
    {}

    ~TLane() {
        TRowSet rowSet{};
        while (Queue.TryPop(rowSet)) {
            Release(&rowSet);
        }
    }

    TSPSC<TRowSet> Queue;
    std::atomic<bool> Finished = false;
};

TGatherConnection::TGatherConnection(size_t capacity)
    : Capacity_(capacity)
{
    Resize(1, 1);
}

TGatherConnection::~TGatherConnection() = default;

EConnectionKind TGatherConnection::Kind() const {
    return EConnectionKind::Gather;
}

void TGatherConnection::Resize(size_t srcCount, size_t dstCount) {
    assert(dstCount == 1);
    Size_ = srcCount;
    FetchId_ = 0;
    FinishedCount_.store(0, std::memory_order_release);
    Lanes_.clear();
    Lanes_.reserve(Size_);
    for (size_t i = 0; i < Size_; ++i) {
        Lanes_.push_back(std::make_unique<TLane>(Capacity_));
    }
}

size_t TGatherConnection::SrcCount() const {
    return Size_;
}

size_t TGatherConnection::DstCount() const {
    return 1;
}

bool TGatherConnection::CanPush(size_t srcId) const {
    assert(srcId < Size_);
    return Lanes_[srcId]->Queue.CanPush();
}

bool TGatherConnection::Push(size_t srcId, TRowSet&& rowSet) {
    assert(srcId < Size_);
    auto moved = TakeRowSet(rowSet);
    if (Lanes_[srcId]->Queue.TryPush(std::move(moved))) {
        return true;
    }
    rowSet = TakeRowSet(moved);
    return false;
}

void TGatherConnection::Finish(size_t srcId) {
    assert(srcId < Size_);
    if (!Lanes_[srcId]->Finished.exchange(true, std::memory_order_acq_rel)) {
        FinishedCount_.fetch_add(1, std::memory_order_acq_rel);
    }
}

EFetchResult TGatherConnection::Fetch(size_t dstId, TRowSet& rowSet) {
    assert(dstId == 0);
    if (Size_ == 0) {
        return EFetchResult::FINISHED;
    }

    auto from = (FetchId_ + 1) % Size_;
    FetchId_ = from;
    for (size_t i = 0; i < Size_; ++i) {
        auto index = (from + i) % Size_;
        if (Lanes_[index]->Queue.TryPop(rowSet)) {
            return EFetchResult::OK;
        }
    }

    return (FinishedCount_.load(std::memory_order_acquire) == Size_)
        ? EFetchResult::FINISHED
        : EFetchResult::NO_DATA;
}

} // namespace NScheduler
} // namespace NQdb
