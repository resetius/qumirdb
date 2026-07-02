#pragma once

#include <qdb/io/io.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace NQdb {
namespace NScheduler {

enum class EConnectionKind {
    OneToOne = 0,
    Gather = 1,
    HashShuffle = 2,
    Broadcast = 3,
};

enum class EFetchResult {
    OK = 0,
    NO_DATA = 1,
    FINISHED = 2,
};

struct TConnectionStats {
    uint64_t Pushed = 0;
    uint64_t Popped = 0;
    uint64_t Finished = 0;
    uint64_t BlockedPush = 0;
    uint64_t EmptyFetch = 0;
    uint64_t FinishedFetch = 0;
};

class IConnection {
public:
    virtual ~IConnection() = default;

    virtual EConnectionKind Kind() const = 0;
    virtual void Resize(size_t srcCount, size_t dstCount) = 0;
    virtual size_t SrcCount() const = 0;
    virtual size_t DstCount() const = 0;

    virtual bool CanPush(size_t srcId) const = 0;
    virtual bool Push(size_t srcId, TRowSet&& rowSet) = 0;
    virtual void Finish(size_t srcId) = 0;

    virtual EFetchResult Fetch(size_t dstId, TRowSet& rowSet) = 0;

    void SetDebugName(std::string name);
    const std::string& DebugName() const;
    void SetStatsEnabled(bool enabled);
    TConnectionStats Stats() const;

protected:
    void CountPushed(uint64_t count = 1) const;
    void CountPopped(uint64_t count = 1) const;
    void CountFinished(uint64_t count = 1) const;
    void CountBlockedPush(uint64_t count = 1) const;
    void CountEmptyFetch(uint64_t count = 1) const;
    void CountFinishedFetch(uint64_t count = 1) const;

private:
    std::string DebugName_;
    bool StatsEnabled_ = false;
    mutable std::atomic<uint64_t> Pushed_ = 0;
    mutable std::atomic<uint64_t> Popped_ = 0;
    mutable std::atomic<uint64_t> Finished_ = 0;
    mutable std::atomic<uint64_t> BlockedPush_ = 0;
    mutable std::atomic<uint64_t> EmptyFetch_ = 0;
    mutable std::atomic<uint64_t> FinishedFetch_ = 0;
};

class TOneToOneConnection : public IConnection {
public:
    explicit TOneToOneConnection(size_t capacity = 1);
    ~TOneToOneConnection() override;

    EConnectionKind Kind() const override;
    void Resize(size_t srcCount, size_t dstCount) override;
    size_t SrcCount() const override;
    size_t DstCount() const override;

    bool CanPush(size_t srcId) const override;
    bool Push(size_t srcId, TRowSet&& rowSet) override;
    void Finish(size_t srcId) override;

    EFetchResult Fetch(size_t dstId, TRowSet& rowSet) override;

private:
    struct TLane;

    size_t Capacity_;
    size_t Size_ = 0;
    std::vector<std::unique_ptr<TLane>> Lanes_;
};

class TGatherConnection : public IConnection {
public:
    explicit TGatherConnection(size_t capacity = 1);
    ~TGatherConnection() override;

    EConnectionKind Kind() const override;
    void Resize(size_t srcCount, size_t dstCount) override;
    size_t SrcCount() const override;
    size_t DstCount() const override;

    bool CanPush(size_t srcId) const override;
    bool Push(size_t srcId, TRowSet&& rowSet) override;
    void Finish(size_t srcId) override;

    EFetchResult Fetch(size_t dstId, TRowSet& rowSet) override;

private:
    struct TLane;

    size_t Capacity_;
    size_t Size_ = 0;
    size_t FetchId_ = 0;
    std::atomic<size_t> FinishedCount_ = 0;
    std::vector<std::unique_ptr<TLane>> Lanes_;
};

class THashShuffleConnection : public IConnection {
public:
    explicit THashShuffleConnection(size_t capacity = 1);
    ~THashShuffleConnection() override;

    EConnectionKind Kind() const override;
    void Resize(size_t srcCount, size_t dstCount) override;
    size_t SrcCount() const override;
    size_t DstCount() const override;

    bool CanPush(size_t srcId) const override;
    bool Push(size_t srcId, TRowSet&& rowSet) override;
    void Finish(size_t srcId) override;

    bool CanPushTo(size_t srcId, size_t dstId) const;
    bool PushTo(size_t srcId, size_t dstId, TRowSet&& rowSet);

    EFetchResult Fetch(size_t dstId, TRowSet& rowSet) override;

private:
    struct TLane;

    size_t LaneIndex(size_t srcId, size_t dstId) const;

private:
    size_t Capacity_;
    size_t SrcCount_ = 0;
    size_t DstCount_ = 0;
    std::vector<std::unique_ptr<TLane>> Lanes_;
    std::vector<std::unique_ptr<std::atomic<bool>>> Finished_;
    std::atomic<size_t> FinishedCount_ = 0;
    std::vector<size_t> FetchIds_;
};

// One producer, N consumers: every pushed rowset is replicated (shared) to all
// destination lanes. Used to broadcast a small side (e.g. a scalar) to every
// partition of the vector side in a cross join.
class TBroadcastConnection : public IConnection {
public:
    explicit TBroadcastConnection(size_t capacity = 1);
    ~TBroadcastConnection() override;

    EConnectionKind Kind() const override;
    void Resize(size_t srcCount, size_t dstCount) override;
    size_t SrcCount() const override;
    size_t DstCount() const override;

    bool CanPush(size_t srcId) const override;
    bool Push(size_t srcId, TRowSet&& rowSet) override;
    void Finish(size_t srcId) override;

    EFetchResult Fetch(size_t dstId, TRowSet& rowSet) override;

private:
    struct TLane;

    size_t Capacity_;
    size_t DstCount_ = 0;
    std::atomic<bool> Finished_ = false;
    std::vector<std::unique_ptr<TLane>> Lanes_;
};

} // namespace NScheduler
} // namespace NQdb
