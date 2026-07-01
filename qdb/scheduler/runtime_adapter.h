#pragma once

#include <qdb/io/io.h>
#include <qdb/scheduler/connection.h>
#include <qdb/scheduler/state.h>

#include <functional>
#include <memory>

namespace NQdb {
namespace NScheduler {

struct TInputPort {
    IConnection* Connection = nullptr;
    size_t Lane = 0;

    EFetchResult Fetch(TRowSet& rowSet) const;
};

struct TOutputPort {
    IConnection* Connection = nullptr;
    size_t Lane = 0;

    bool CanPush() const;
    bool Push(TRowSet&& rowSet) const;
    void Finish() const;
};

struct TSourceCode {
    using TNext = std::function<bool(void*, TRowSet&)>;

    explicit TSourceCode(TNext next);

    TNext Next;
};

struct TUnaryCode {
    using TProcess = std::function<void(void*, TRowSet&)>;

    explicit TUnaryCode(TProcess process);

    TProcess Process;
};

struct TSinkCode {
    using TWrite = std::function<void(void*, const TRowSet&)>;

    explicit TSinkCode(TWrite write);

    TWrite Write;
};

struct TBlockingCode {
    using TProcess = std::function<ETaskResult(void*, TInputPort&, TRowSet&)>;

    explicit TBlockingCode(TProcess process);

    TProcess Process;
};

class TSourceTask final : public ITaskNode {
public:
    TSourceTask(
        std::shared_ptr<const TSourceCode> code,
        std::shared_ptr<void> state,
        TOutputPort output);

    ETaskResult Execute() override;

    const std::shared_ptr<const TSourceCode>& Code() const;
    const std::shared_ptr<void>& State() const;

private:
    std::shared_ptr<const TSourceCode> Code_;
    std::shared_ptr<void> State_;
    TOutputPort Output_;
    bool Finished_ = false;
};

class TUnaryTask final : public ITaskNode {
public:
    TUnaryTask(
        std::shared_ptr<const TUnaryCode> code,
        std::shared_ptr<void> state,
        TInputPort input,
        TOutputPort output);
    ~TUnaryTask() override;

    ETaskResult Execute() override;

    const std::shared_ptr<const TUnaryCode>& Code() const;
    const std::shared_ptr<void>& State() const;

private:
    std::shared_ptr<const TUnaryCode> Code_;
    std::shared_ptr<void> State_;
    TInputPort Input_;
    TOutputPort Output_;
    TRowSet CurrentInput_ = {};
    bool HasInput_ = false;
    bool InputFinished_ = false;
    bool OutputFinished_ = false;
};

class TSinkTask final : public ITaskNode {
public:
    TSinkTask(
        std::shared_ptr<const TSinkCode> code,
        std::shared_ptr<void> state,
        TInputPort input);

    ETaskResult Execute() override;

    const std::shared_ptr<const TSinkCode>& Code() const;
    const std::shared_ptr<void>& State() const;

private:
    std::shared_ptr<const TSinkCode> Code_;
    std::shared_ptr<void> State_;
    TInputPort Input_;
    bool Finished_ = false;
};

class TBlockingTask final : public ITaskNode {
public:
    TBlockingTask(
        std::shared_ptr<const TBlockingCode> code,
        std::shared_ptr<void> state,
        TInputPort input,
        TOutputPort output);
    ~TBlockingTask() override;

    ETaskResult Execute() override;

    const std::shared_ptr<const TBlockingCode>& Code() const;
    const std::shared_ptr<void>& State() const;

private:
    std::shared_ptr<const TBlockingCode> Code_;
    std::shared_ptr<void> State_;
    TInputPort Input_;
    TOutputPort Output_;
    TRowSet CurrentOutput_ = {};
    bool HasOutput_ = false;
    bool OutputFinished_ = false;
};

} // namespace NScheduler
} // namespace NQdb
