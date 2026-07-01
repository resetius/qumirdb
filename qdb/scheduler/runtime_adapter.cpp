#include <qdb/scheduler/runtime_adapter.h>

#include <utility>

namespace NQdb {
namespace NScheduler {

EFetchResult TInputPort::Fetch(TRowSet& rowSet) const {
    return Connection
        ? Connection->Fetch(Lane, rowSet)
        : EFetchResult::FINISHED;
}

bool TOutputPort::CanPush() const {
    return !Connection || Connection->CanPush(Lane);
}

bool TOutputPort::Push(TRowSet&& rowSet) const {
    return Connection
        ? Connection->Push(Lane, std::move(rowSet))
        : true;
}

void TOutputPort::Finish() const {
    if (Connection) {
        Connection->Finish(Lane);
    }
}

TSourceCode::TSourceCode(TNext next)
    : Next(std::move(next))
{}

TUnaryCode::TUnaryCode(TProcess process)
    : Process(std::move(process))
{}

TSinkCode::TSinkCode(TWrite write)
    : Write(std::move(write))
{}

TBlockingCode::TBlockingCode(TProcess process)
    : Process(std::move(process))
{}

TBinaryBlockingCode::TBinaryBlockingCode(TProcess process)
    : Process(std::move(process))
{}

TSourceTask::TSourceTask(
    std::shared_ptr<const TSourceCode> code,
    std::shared_ptr<void> state,
    TOutputPort output)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Output_(output)
{}

ETaskResult TSourceTask::Execute() {
    if (Finished_) {
        return ETaskResult::FINISHED;
    }
    if (!Output_.CanPush()) {
        return ETaskResult::BLOCKED_OUTPUT;
    }

    TRowSet rowSet{};
    if (!Code_->Next(State_.get(), rowSet)) {
        Finished_ = true;
        Output_.Finish();
        return ETaskResult::FINISHED;
    }

    if (!Output_.Push(std::move(rowSet))) {
        Release(&rowSet);
        return ETaskResult::BLOCKED_OUTPUT;
    }
    return ETaskResult::OK;
}

const std::shared_ptr<const TSourceCode>& TSourceTask::Code() const {
    return Code_;
}

const std::shared_ptr<void>& TSourceTask::State() const {
    return State_;
}

TUnaryTask::TUnaryTask(
    std::shared_ptr<const TUnaryCode> code,
    std::shared_ptr<void> state,
    TInputPort input,
    TOutputPort output)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Input_(input)
    , Output_(output)
{}

TUnaryTask::~TUnaryTask() {
    if (HasInput_) {
        Release(&CurrentInput_);
    }
}

ETaskResult TUnaryTask::Execute() {
    if (OutputFinished_) {
        return ETaskResult::FINISHED;
    }

    if (!HasInput_ && !InputFinished_) {
        auto fetch = Input_.Fetch(CurrentInput_);
        if (fetch == EFetchResult::OK) {
            HasInput_ = true;
        } else if (fetch == EFetchResult::FINISHED) {
            InputFinished_ = true;
        } else {
            return ETaskResult::NEED_DATA;
        }
    }

    if (!HasInput_) {
        OutputFinished_ = true;
        Output_.Finish();
        return ETaskResult::FINISHED;
    }

    if (!Output_.CanPush()) {
        return ETaskResult::BLOCKED_OUTPUT;
    }

    Code_->Process(State_.get(), CurrentInput_);
    if (!Output_.Push(std::move(CurrentInput_))) {
        return ETaskResult::BLOCKED_OUTPUT;
    }
    CurrentInput_ = {};
    HasInput_ = false;
    return ETaskResult::OK;
}

const std::shared_ptr<const TUnaryCode>& TUnaryTask::Code() const {
    return Code_;
}

const std::shared_ptr<void>& TUnaryTask::State() const {
    return State_;
}

TSinkTask::TSinkTask(
    std::shared_ptr<const TSinkCode> code,
    std::shared_ptr<void> state,
    TInputPort input)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Input_(input)
{}

ETaskResult TSinkTask::Execute() {
    if (Finished_) {
        return ETaskResult::FINISHED;
    }

    TRowSet rowSet{};
    auto fetch = Input_.Fetch(rowSet);
    if (fetch == EFetchResult::NO_DATA) {
        return ETaskResult::NEED_DATA;
    }
    if (fetch == EFetchResult::FINISHED) {
        Finished_ = true;
        return ETaskResult::FINISHED;
    }

    Code_->Write(State_.get(), rowSet);
    Release(&rowSet);
    return ETaskResult::OK;
}

const std::shared_ptr<const TSinkCode>& TSinkTask::Code() const {
    return Code_;
}

const std::shared_ptr<void>& TSinkTask::State() const {
    return State_;
}

TBlockingTask::TBlockingTask(
    std::shared_ptr<const TBlockingCode> code,
    std::shared_ptr<void> state,
    TInputPort input,
    TOutputPort output)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Input_(input)
    , Output_(output)
{}

TBlockingTask::~TBlockingTask() {
    if (HasOutput_) {
        Release(&CurrentOutput_);
    }
}

ETaskResult TBlockingTask::Execute() {
    if (OutputFinished_) {
        return ETaskResult::FINISHED;
    }

    if (HasOutput_) {
        if (!Output_.CanPush()) {
            return ETaskResult::BLOCKED_OUTPUT;
        }
        if (!Output_.Push(std::move(CurrentOutput_))) {
            return ETaskResult::BLOCKED_OUTPUT;
        }
        CurrentOutput_ = {};
        HasOutput_ = false;
        return ETaskResult::OK;
    }

    auto result = Code_->Process(State_.get(), Input_, CurrentOutput_);
    if (result == ETaskResult::OK) {
        HasOutput_ = true;
        return ETaskResult::OK;
    }
    if (result == ETaskResult::FINISHED) {
        OutputFinished_ = true;
        Output_.Finish();
    }
    return result;
}

const std::shared_ptr<const TBlockingCode>& TBlockingTask::Code() const {
    return Code_;
}

const std::shared_ptr<void>& TBlockingTask::State() const {
    return State_;
}

TBinaryBlockingTask::TBinaryBlockingTask(
    std::shared_ptr<const TBinaryBlockingCode> code,
    std::shared_ptr<void> state,
    TInputPort left,
    TInputPort right,
    TOutputPort output)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Left_(left)
    , Right_(right)
    , Output_(output)
{}

TBinaryBlockingTask::~TBinaryBlockingTask() {
    if (HasOutput_) {
        Release(&CurrentOutput_);
    }
}

ETaskResult TBinaryBlockingTask::Execute() {
    if (OutputFinished_) {
        return ETaskResult::FINISHED;
    }

    if (HasOutput_) {
        if (!Output_.CanPush()) {
            return ETaskResult::BLOCKED_OUTPUT;
        }
        if (!Output_.Push(std::move(CurrentOutput_))) {
            return ETaskResult::BLOCKED_OUTPUT;
        }
        CurrentOutput_ = {};
        HasOutput_ = false;
        return ETaskResult::OK;
    }

    auto result = Code_->Process(State_.get(), Left_, Right_, CurrentOutput_);
    if (result == ETaskResult::OK) {
        HasOutput_ = true;
        return ETaskResult::OK;
    }
    if (result == ETaskResult::FINISHED) {
        OutputFinished_ = true;
        Output_.Finish();
    }
    return result;
}

const std::shared_ptr<const TBinaryBlockingCode>& TBinaryBlockingTask::Code() const {
    return Code_;
}

const std::shared_ptr<void>& TBinaryBlockingTask::State() const {
    return State_;
}

} // namespace NScheduler
} // namespace NQdb
