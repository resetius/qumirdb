#pragma once

#include <qdb/io/io.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace NQdb {
namespace NScheduler {

enum class EConnectionKind {
    OneToOne = 0,
    Gather = 1,
};

enum class EFetchResult {
    OK = 0,
    NO_DATA = 1,
    FINISHED = 2,
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

} // namespace NScheduler
} // namespace NQdb
