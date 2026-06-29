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

enum class EConnectionKind {
    OneToOne,    // i -> i
    Gather,      // many -> one, unordered
    Merge,       // many -> one, sorted
    HashShuffle, // many -> many split by hash(keys)
    Broadcast,   // one/many -> all
    RoundRobin,  // many -> many in random order
};

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

struct IConnection {
    virtual ~IConnection() = default;

    // output side
    virtual bool CanPush(int src_id) const = 0;
    virtual void Push(int src_id, TRowSet&& rowSet) = 0;
    virtual void Finish(int src_id) = 0;

    // input side
    virtual EFetchState Fetch(int dst_id, TRowSet& batch) = 0;
};

struct TInputPort {
    IConnection* Conn = nullptr;
    int DstId = 0;

    EFetchState Fetch(TRowSet& batch) {
        return Conn->Fetch(DstId, batch);
    }
};

struct TOutputPort {
    IConnection* Conn = nullptr;
    int SrcId = 0;

    bool CanPush() const {
        if (!Conn) {
            return true;
        }
        return Conn->CanPush(SrcId);
    }

    void Push(TRowSet&& rowSet) {
        if (Conn) {
            Conn->Push(SrcId, std::move(rowSet));
        }
    }

    void Finish() {
        if (Conn) {
            Conn->Finish(SrcId);
        }
    }
};

static constexpr int MaxOutput = 1;

struct TOneToOne : IConnection {
    // Output
    bool CanPush(int src_id) const override {
        assert(src_id == 0);
        return (Output_.size() < MaxOutput);
    }

    void Push(int src_id, TRowSet&& rowSet) override {
        assert(src_id == 0);
        Output_.emplace_back(std::move(rowSet));
    }

    void Finish(int src_id) override {
        assert(src_id == 0);
        Finished_ = true;
    }

    // Input
    EFetchState Fetch(int dst_id, TRowSet& rowSet) override {
        assert(dst_id == 0);
        if (!Output_.empty()) {
            rowSet = std::move(Output_.front()); Output_.pop_front();
            return EFetchState::OK;
        }

        return Finished_ ? EFetchState::FINISHED : EFetchState::NO_DATA;
    }

    bool Finished_ = false;
    std::deque<TRowSet> Output_;
};

struct TGather : IConnection {
    void Push(int src_id, TRowSet&& rowSet) {
    }

    EFetchState Fetch(int dst_id, TRowSet& batch) {
        assert(dst_id == 0);
    }
};

struct TMerge : IConnection {
    void Push(int src_id, TRowSet&& rowSet) {
    }

    EFetchState Fetch(int dst_id, TRowSet& batch) {
        assert(dst_id == 0);
    }
};

struct THashShuffle : IConnection {
    THashShuffle() { }

    void Push(int src_id, TRowSet&& rowSet) {
    }

    EFetchState Fetch(int dst_id, TRowSet& batch) {
    }
};

struct IRuntimeNode {
    virtual ~IRuntimeNode() = default;
    virtual bool Next(TRowSet& rowSet) = 0;

    virtual EState Execute() = 0;

    virtual bool IsFinished() const = 0;
    virtual void Run(TRowSet& rowSet) = 0;

    virtual IRuntimeNode* InputNode() = 0;

    // connections
    virtual void SetInputPort(TInputPort inputPort) = 0;
    virtual void SetOutputPort(TOutputPort outputPort) = 0;
};

void Connect(IRuntimeNode* src, IRuntimeNode* dst, EConnectionKind kind)
{
    switch (kind) {
    case EConnectionKind::OneToOne: {
        // TODO: owner
        auto* conn = new TOneToOne();
        src->SetOutputPort(TOutputPort(conn, 0));
        dst->SetInputPort(TInputPort(conn, 0));
        break;
    }
    default:
        break;
    };
}

struct TNode : virtual IRuntimeNode {
    TInputPort InputPort_;
    TOutputPort OutputPort_;

    IRuntimeNode* InputNode() override {
        return nullptr;
    }

    // connection
    void SetInputPort(TInputPort inputPort) override {
        InputPort_ = std::move(inputPort);
    }

    void SetOutputPort(TOutputPort outputPort) override {
        OutputPort_ = std::move(outputPort);
    }
};

struct TNodeWithOutput : virtual TNode {
    bool Next(TRowSet& rowSet) override {
        if (IsFinished()) {
            return false;
        }

        Run(rowSet);
        return true;
    }
};

struct TNodeWithInput : virtual TNode {
    TNodeWithInput(IRuntimeNode* input)
        : InputNode_(input)
    { }

    IRuntimeNode* InputNode() override {
        return InputNode_;
    }

    bool IsFinished() const override {
        return InputConsumed_;
    }

    EState Execute() override {
        TRowSet rowSet;
        if (!CurrentInput_) {
            auto state = InputPort_.Fetch(rowSet);
            if (state == EFetchState::OK) {
                CurrentInput_.emplace(std::move(rowSet));
            } else if (state == EFetchState::FINISHED) {
                InputConsumed_ = true;
            }
        }

        if (!CurrentInput_) {
            if (InputConsumed_) {
                OutputPort_.Finish();
                return EState::FINISHED;
            }
            return EState::NEED_DATA;
        }

        if (!OutputPort_.CanPush()) {
            return EState::BLOCKED_OUTPUT;
        }

        rowSet = std::move(*CurrentInput_); CurrentInput_ = std::nullopt;
        Run(rowSet);
        OutputPort_.Push(std::move(rowSet));

        if (InputConsumed_) {
            OutputPort_.Finish();
            return EState::FINISHED;
        }
        return EState::OK;
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
    std::optional<TRowSet> CurrentInput_;
};

struct TNodeWithInputOutput : virtual TNodeWithInput, virtual TNodeWithOutput {
    TNodeWithInputOutput(IRuntimeNode* input)
    {
        InputNode_ = input;
    }

    bool Next(TRowSet& rowSet) override {
        return TNodeWithInput::Next(rowSet);
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
            OutputPort_.Finish();
            return EState::FINISHED;
        }
        if (!OutputPort_.CanPush()) {
            return EState::BLOCKED_OUTPUT;
        }
        TRowSet rowSet;
        Run(rowSet);
        OutputPort_.Push(std::move(rowSet));

        if (IsFinished()) {
            OutputPort_.Finish();
            return EState::FINISHED;
        }
        return EState::OK;
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
    { }

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
    { }

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
    std::unordered_map<IRuntimeNode*, std::vector<IRuntimeNode*>> Inbound;
    std::unordered_map<IRuntimeNode*, std::vector<IRuntimeNode*>> Outbound;

    TScheduler(
        std::unordered_map<IRuntimeNode*, std::vector<IRuntimeNode*>> inbound,
        std::unordered_map<IRuntimeNode*, std::vector<IRuntimeNode*>> outbound)
        : Inbound(std::move(inbound))
        , Outbound(std::move(outbound))
    { }

    void Schedule(IRuntimeNode* node) {
        if (node != nullptr && !Scheduled.contains(node)) {
            Ready.emplace_back(node);
            Scheduled.emplace(node);
        }
    }

    void ScheduleInput(IRuntimeNode* node) {
        auto it = Inbound.find(node);
        if (it == Inbound.end()) {
            return;
        }
        for (auto& node : it->second) {
            Schedule(node);
        }
    }

    void ScheduleOutput(IRuntimeNode* node) {
        auto it = Outbound.find(node);
        if (it == Outbound.end()) {
            return;
        }
        for (auto& node : it->second) {
            Schedule(node);
        }
    }

    void Run() {
        while (!Ready.empty()) {
            auto node = Ready.front(); Ready.pop_front();
            Scheduled.erase(node);
            auto state = node->Execute();
            if (state == EState::NEED_DATA) {
                ScheduleInput(node);
            } else if (state == EState::BLOCKED_OUTPUT || state == EState::FINISHED) {
                ScheduleOutput(node);
            } else if (state == EState::OK) {
                Schedule(node);
                ScheduleOutput(node);
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
    auto filter = std::make_unique<TFilter>(43, 30, nullptr);
    auto printer = std::make_unique<TPrinter>(nullptr);

    Connect(scan.get(), filter.get(), EConnectionKind::OneToOne);
    Connect(filter.get(), printer.get(), EConnectionKind::OneToOne);

    std::unordered_map<IRuntimeNode*, std::vector<IRuntimeNode*>> Inbound;
    std::unordered_map<IRuntimeNode*, std::vector<IRuntimeNode*>> Outbound;

    Inbound[filter.get()].push_back(scan.get());
    Inbound[printer.get()].push_back(filter.get());

    Outbound[scan.get()].push_back(filter.get());
    Outbound[filter.get()].push_back(printer.get());

    TScheduler scheduler(Inbound, Outbound);
    scheduler.Schedule(printer.get());
    scheduler.Run();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}