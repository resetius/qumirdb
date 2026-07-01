#include <qdb/scheduler/runtime_adapter.h>

#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace NQdb {
namespace NScheduler {
namespace {

struct THashShuffleRowSetData {
    std::shared_ptr<TRowSet> Input;
    std::vector<uint8_t> Selection;
};

void DestroyHashShuffleRowSet(TRowSet* rowSet) {
    auto* data = static_cast<THashShuffleRowSetData*>(rowSet->Private);
    delete data;
}

void DestroySharedRowSet(TRowSet* rowSet) {
    Release(rowSet);
    delete rowSet;
}

bool RowSelected(const TRowSet& rowSet, int64_t row) {
    return !rowSet.Selection || rowSet.Selection[row] != 0;
}

} // namespace

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

THashShuffleCode::THashShuffleCode(THash hash)
    : Hash(std::move(hash))
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

struct THashShuffleTask::TPendingOutput {
    size_t DstLane = 0;
    TRowSet RowSet = {};
};

THashShuffleTask::THashShuffleTask(
    std::shared_ptr<const THashShuffleCode> code,
    std::shared_ptr<void> state,
    TInputPort input,
    THashShuffleConnection& output,
    size_t sourceLane)
    : Code_(std::move(code))
    , State_(std::move(state))
    , Input_(input)
    , Output_(&output)
    , SourceLane_(sourceLane)
{}

THashShuffleTask::~THashShuffleTask() {
    ClearPending();
}

ETaskResult THashShuffleTask::Execute() {
    if (OutputFinished_) {
        return ETaskResult::FINISHED;
    }

    if (PendingIndex_ < Pending_.size()) {
        return PushPending();
    }

    TRowSet rowSet{};
    auto fetch = Input_.Fetch(rowSet);
    if (fetch == EFetchResult::NO_DATA) {
        return ETaskResult::NEED_DATA;
    }
    if (fetch == EFetchResult::FINISHED) {
        OutputFinished_ = true;
        Output_->Finish(SourceLane_);
        return ETaskResult::FINISHED;
    }

    Scatter(rowSet);
    if (rowSet.RefCount) {
        Release(&rowSet);
    }
    return PushPending();
}

const std::shared_ptr<const THashShuffleCode>& THashShuffleTask::Code() const {
    return Code_;
}

const std::shared_ptr<void>& THashShuffleTask::State() const {
    return State_;
}

void THashShuffleTask::Scatter(TRowSet& rowSet) {
    const size_t partitions = Output_->DstCount();
    if (partitions == 0 || rowSet.RowCount <= 0) {
        return;
    }

    // Many-to-one shuffle: every row routes to lane 0, so skip hashing and
    // forward the whole rowset (keeping its existing selection) unchanged.
    if (partitions == 1) {
        ClearPending();
        auto input = std::shared_ptr<TRowSet>(new TRowSet(rowSet), DestroySharedRowSet);
        rowSet = {};
        auto* data = new THashShuffleRowSetData;
        data->Input = input;
        Pending_.push_back(TPendingOutput{
            .DstLane = 0,
            .RowSet = TRowSet{
                .Columns = input->Columns,
                .ColumnCount = input->ColumnCount,
                .RowCount = input->RowCount,
                .Selection = input->Selection,
                .Destroy = DestroyHashShuffleRowSet,
                .Private = data,
                .RefCount = 1,
            },
        });
        return;
    }

    Hashes_.assign(static_cast<size_t>(rowSet.RowCount), 0);
    if (!Code_->Hash(&rowSet, Hashes_.data())) {
        throw std::runtime_error("hash shuffle hash kernel failed");
    }

    std::vector<std::vector<uint8_t>> selections(partitions);
    std::vector<size_t> counts(partitions, 0);
    for (auto& selection : selections) {
        selection.assign(static_cast<size_t>(rowSet.RowCount), uint8_t{0});
    }
    for (int64_t row = 0; row < rowSet.RowCount; ++row) {
        if (!RowSelected(rowSet, row)) {
            continue;
        }
        const size_t dst = static_cast<size_t>(Hashes_[static_cast<size_t>(row)] % partitions);
        selections[dst][static_cast<size_t>(row)] = 0xff;
        ++counts[dst];
    }

    ClearPending();
    auto input = std::shared_ptr<TRowSet>(new TRowSet(rowSet), DestroySharedRowSet);
    rowSet = {};

    Pending_.reserve(partitions);
    for (size_t dst = 0; dst < partitions; ++dst) {
        if (counts[dst] == 0) {
            continue;
        }
        auto* data = new THashShuffleRowSetData;
        data->Input = input;
        data->Selection = std::move(selections[dst]);
        Pending_.push_back(TPendingOutput{
            .DstLane = dst,
            .RowSet = TRowSet{
                .Columns = input->Columns,
                .ColumnCount = input->ColumnCount,
                .RowCount = input->RowCount,
                .Selection = data->Selection.data(),
                .Destroy = DestroyHashShuffleRowSet,
                .Private = data,
                .RefCount = 1,
            },
        });
    }
}

ETaskResult THashShuffleTask::PushPending() {
    while (PendingIndex_ < Pending_.size()) {
        auto& pending = Pending_[PendingIndex_];
        if (!Output_->CanPushTo(SourceLane_, pending.DstLane)) {
            return ETaskResult::BLOCKED_OUTPUT;
        }
        if (!Output_->PushTo(SourceLane_, pending.DstLane, std::move(pending.RowSet))) {
            return ETaskResult::BLOCKED_OUTPUT;
        }
        ++PendingIndex_;
    }
    ClearPending();
    return ETaskResult::OK;
}

void THashShuffleTask::ClearPending() {
    for (size_t i = PendingIndex_; i < Pending_.size(); ++i) {
        if (Pending_[i].RowSet.RefCount) {
            Release(&Pending_[i].RowSet);
        }
    }
    Pending_.clear();
    PendingIndex_ = 0;
}

} // namespace NScheduler
} // namespace NQdb
