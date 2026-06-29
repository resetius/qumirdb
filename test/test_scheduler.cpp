#include <gtest/gtest.h>

#include <deque>
#include <vector>
#include <list>
#include <unordered_set>

namespace {

int RandomInt(int* seed) {
    *seed = (*seed * 1103515245 + 12345) & 0x7fffffff;
    return *seed;
}

struct TRowSet {
    std::vector<std::string> Col;
    std::vector<int> Selection;

    bool Empty() const {
        return Col.empty();
    }
};

void GenerateKey(std::string& key, int* seed) {
    int len = 2 + RandomInt(seed) % 5;
    key.resize(len);
    for (int i = 0; i < len; i++) {
        key[i] = 'a' + RandomInt(seed) % ('z' - 'a' + 1);
    }
}

void GenerateRowSet(TRowSet& rowSet, int size, int* seed) {
    rowSet.Col.clear();
    rowSet.Selection.clear();
    std::string key;
    for (int i = 0; i < size; i++) {
        GenerateKey(key, seed);
        rowSet.Col.push_back(key);
        rowSet.Selection.push_back(1);
    }
}

enum class EState {
    OK = 0,
    NEED_DATA = 1,
    BLOCKED_OUTPUT = 2,
    FINISHED = 3,
};

enum class EFetchState {
    OK = 0,
    NO_DATA = 1,
    FINISHED = 2,
};

struct IRuntimeNode {
    virtual ~IRuntimeNode() = default;
    virtual bool Next(TRowSet& rowSet) = 0;

    virtual EFetchState Fetch(TRowSet& rowSet) = 0;
    virtual EState Execute() = 0;

    virtual bool IsFinished() const = 0;
    virtual void Run(TRowSet& rowSet) = 0;

    virtual IRuntimeNode* InputNode() = 0;
    virtual IRuntimeNode* OutputNode() = 0;

    virtual bool CanPushOutput() = 0;
    virtual void PushOutput(TRowSet&& rowSet) = 0;
    virtual void SetOutputNode(IRuntimeNode* node) = 0;
};

struct TNode : virtual IRuntimeNode {
    IRuntimeNode* InputNode() override {
        return nullptr;
    }

    IRuntimeNode* OutputNode() override {
        return nullptr;
    }

    bool CanPushOutput() override {
        return true;
    }

    void PushOutput(TRowSet&& rowSet) override
    { }

    void SetOutputNode(IRuntimeNode* node)  override
    { }
};

struct TNodeWithOutput : virtual TNode {
    IRuntimeNode* OutputNode() override {
        return OutputNode_;
    }

    void SetOutputNode(IRuntimeNode* node) override {
        OutputNode_ = node;
    }

    EFetchState Fetch(TRowSet& rowSet) override {
        if (!Output_.empty()) {
            rowSet = std::move(Output_.front()); Output_.pop_front();
            return EFetchState::OK;
        }

        return IsFinished() ? EFetchState::FINISHED : EFetchState::NO_DATA;
    }

    bool CanPushOutput() override {
        return Output_.size() < MaxOutput_;
    }

    void PushOutput(TRowSet&& rowSet) override
    {
        Output_.emplace_back(std::move(rowSet));
    }

    bool Next(TRowSet& rowSet) override {
        if (IsFinished()) {
            return false;
        }

        Run(rowSet);
        return true;
    }

    IRuntimeNode* OutputNode_ = nullptr;
    const bool MaxOutput_ = 1; // TODO
    std::deque<TRowSet> Output_;
};

struct TNodeWithInput : virtual TNode {
    TNodeWithInput(IRuntimeNode* input)
        : InputNode_(input)
    { }

    IRuntimeNode* InputNode() override {
        return InputNode_;
    }

    EFetchState Fetch(TRowSet& rowSet) override {
        return IsFinished() ? EFetchState::FINISHED : EFetchState::NO_DATA;
    }

    bool IsFinished() const override {
        return InputConsumed_;
    }

    EState Execute() override {
        TRowSet rowSet;
        if (Input_.empty()) {
            auto state = InputNode_->Fetch(rowSet);
            if (state == EFetchState::OK) {
                Input_.emplace_back(std::move(rowSet));
            } else if (state == EFetchState::FINISHED) {
                InputConsumed_ = true;
            }
        }

        if (Input_.empty()) {
            return InputConsumed_ ? EState::FINISHED : EState::NEED_DATA;
        }

        rowSet = std::move(Input_.front()); Input_.pop_front();
        Run(rowSet);

        if (CanPushOutput()) {
            PushOutput(std::move(rowSet));
        } else {
            Input_.emplace_front(std::move(rowSet));
            return EState::BLOCKED_OUTPUT;
        }

        return InputConsumed_ ? EState::FINISHED : EState::OK;
    }

    bool Next(TRowSet& rowSet) override {
        if (!InputNode_->Next(rowSet)) {
            return false;
        }

        Run(rowSet);

        return true;
    }

    IRuntimeNode* InputNode_ = nullptr;
    bool InputConsumed_ = false;
    std::deque<TRowSet> Input_;
};

struct TNodeWithInputOutput : virtual TNodeWithInput, virtual TNodeWithOutput {
    TNodeWithInputOutput(IRuntimeNode* input)
    {
        InputNode_ = input;
    }

    bool Next(TRowSet& rowSet) override {
        return TNodeWithInput::Next(rowSet);
    }

    EFetchState Fetch(TRowSet& rowSet) override {
        return TNodeWithOutput::Fetch(rowSet);
    }
};

struct TScan : virtual TNodeWithOutput {
    int NSets;
    int Seed;

    int Ncols = 16;

    int SetId = 0;

    TScan(int nsets, int seed)
        : NSets(nsets)
        , Seed(seed)
    { }

    bool IsFinished() const override {
        return SetId == NSets;
    }

    void Run(TRowSet& rowSet) override {
        GenerateRowSet(rowSet, Ncols, &Seed);
        SetId ++;
    }

    EState Execute() override {
        if (IsFinished()) {
            return EState::FINISHED;
        }
        if (!CanPushOutput()) {
            return EState::BLOCKED_OUTPUT;
        }
        Run(Output_.emplace_back());
        return IsFinished() ? EState::FINISHED : EState::OK;
    }
};

struct TFilter : virtual TNodeWithInputOutput {
    int Seed;
    int Percent;

    TFilter(int seed, int percent, IRuntimeNode* input)
        : TNodeWithInput(input)
        , TNodeWithInputOutput(input)
        , Seed(seed)
        , Percent(percent)
    {
        InputNode_->SetOutputNode(this);
    }

    void Run(TRowSet& rowSet) override {
        for (int i = 0; i < rowSet.Col.size(); ++i) {
            if (rowSet.Selection[i]) {
                int k = (RandomInt(&Seed) % 100);
                if (k >= Percent) {
                    rowSet.Selection[i] = 0;
                }
            }
        }
    }
};

struct TPrinter : virtual TNodeWithInput {
    TPrinter(IRuntimeNode* input)
        : TNodeWithInput(input)
    {
        InputNode_->SetOutputNode(this);
    }

    void Run(TRowSet& rowSet) override {
        for (int i = 0; i < rowSet.Col.size(); ++i) {
            if (rowSet.Selection[i]) {
                std::cout << rowSet.Col[i] << "\n";
            }
        }
    }
};

struct TScheduler {
    std::list<IRuntimeNode*> Ready;
    std::unordered_set<IRuntimeNode*> Scheduled;

    TScheduler() { }

    void Schedule(IRuntimeNode* node) {
        if (node != nullptr && !Scheduled.contains(node)) {
            Ready.emplace_back(node);
            Scheduled.emplace(node);
        }
    }

    void Run() {
        while (!Ready.empty()) {
            auto node = Ready.front(); Ready.pop_front();
            Scheduled.erase(node);
            auto state = node->Execute();
            if (state == EState::NEED_DATA) {
                Schedule(node->InputNode());
            } else if (state == EState::BLOCKED_OUTPUT || state == EState::FINISHED) {
                Schedule(node->OutputNode());
            } else if (state == EState::OK) {
                Schedule(node);
                Schedule(node->OutputNode());
            }
        }
    }
};

} // namespace

TEST(Scheduler, Basic) {
    int inputSets = 16;
    auto scan = std::make_unique<TScan>(inputSets, 42);
    auto filter = std::make_unique<TFilter>(43, 30, scan.get());
    auto printer = std::make_unique<TPrinter>(filter.get());

    TRowSet rowSet;
    while (printer->Next(rowSet));
}

TEST(Scheduler, Async) {
    int inputSets = 16;
    auto scan = std::make_unique<TScan>(inputSets, 42);
    auto filter = std::make_unique<TFilter>(43, 30, scan.get());
    auto printer = std::make_unique<TPrinter>(filter.get());

    TScheduler scheduler;
    scheduler.Schedule(printer.get());
    scheduler.Run();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}