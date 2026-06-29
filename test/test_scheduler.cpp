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
    // src_id - номер партиции группы входных тасок
    virtual bool CanPush(int src_id) const = 0;
    virtual void Push(int src_id, TRowSet&& rowSet) = 0;
    virtual void Finish(int src_id) = 0;

    // input side
    // dst_id - номер партиции группы выходных тасок
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

// many -> one
struct TGather : IConnection {
    bool CanPush(int src_id) {
        return Outputs_[src_id].size() < MaxOutput;
    }

    void Finish(int src_id) {
        if (Flags_[src_id] == 0) {
            NFinished_ ++;
        }
        Flags_[src_id] = 1;
    }

    void Push(int src_id, TRowSet&& rowSet) {
        Outputs_[src_id].emplace_back(std::move(rowSet));
    }

    EFetchState Fetch(int dst_id, TRowSet& batch) {
        assert(dst_id == 0);

        auto from = (FetchId_ + 1) % Size_;
        FetchId_ = from;
        for (size_t i = 0; i < Size_; ++i) {
            auto index = (from + i) % Size_;
            if (!Outputs_[index].empty()) {
                batch = std::move(Outputs_[index].front());
                Outputs_[index].pop_front();
                return EFetchState::OK;
            }
        }

        return NFinished_ == Size_
            ? EFetchState::FINISHED
            : EFetchState::NO_DATA;
    }

    size_t Size_;
    size_t FetchId_ = 0;
    size_t NFinished_ = 0;
    std::vector<int> Flags_;
    std::vector<std::deque<TRowSet>> Outputs_;
};

// many -> one
// assume records are sorted
struct TMerge : IConnection {
    bool CanPush(int src_id) {
        return Outputs_[src_id].size() < MaxOutput;
    }

    void Finish(int src_id) {
        if (Flags_[src_id] == 0) {
            NFinished_ ++;
        }
        Flags_[src_id] = 1;
    }

    void Push(int src_id, TRowSet&& rowSet) {
        Outputs_[src_id].emplace_back(std::move(rowSet));
    }

    EFetchState Fetch(int dst_id, TRowSet& batch) {
        assert(dst_id == 0);
    }

    size_t Size_;
    size_t FetchId_ = 0;
    size_t NFinished_ = 0;
    std::vector<int> Flags_;
    std::vector<std::deque<TRowSet>> Outputs_;
};

struct THashShuffle : IConnection {
    bool CanPush(int src_id) {
        // return Outputs_[src_id].size() < MaxOutput;
        // TODO: check size in bytes
        return true;
    }

    void Finish(int src_id) {
        if (Flags_[src_id] == 0) {
            NFinished_ ++;
        }
        Flags_[src_id] = 1;
    }

    void Push(int src_id, TRowSet&& rowSet) {
        // compute hash => distribute accrose outputs
        // hash() % DstSize
    }

    EFetchState Fetch(int dst_id, TRowSet& batch) {
        auto from = (FetchIds_[dst_id] + 1) % SrcSize_;
        FetchIds_[dst_id] = from;
        for (size_t i = 0; i < SrcSize_; ++i) {
            auto index = (from + i) % SrcSize_;
            if (!Outputs_[index].empty()) {
                batch = std::move(Outputs_[index][dst_id].front());
                Outputs_[index][dst_id].pop_front();
                return EFetchState::OK;
            }
        }

        return NFinished_ == SrcSize_
            ? EFetchState::FINISHED
            : EFetchState::NO_DATA;
    }

    size_t SrcSize_;
    size_t DstSize_;
    size_t NFinished_ = 0;

    // FetchIds_.size() == DstSize_
    std::vector<size_t> FetchIds_; // src id to fetch start from
    std::vector<int> Flags_;

    // src_id -> dst_id -> queue
    std::vector<std::vector<std::deque<TRowSet>>> Outputs_;
};

struct IRuntimeNode {
    virtual ~IRuntimeNode() = default;

    virtual EState Execute() = 0;

    virtual bool IsFinished() const = 0;
    virtual void Run(TRowSet& rowSet) = 0;

    // connections
    virtual void SetInputPort(TInputPort inputPort) = 0;
    virtual void SetOutputPort(TOutputPort outputPort) = 0;
};

struct TDagNode {
    std::vector<TDagNode*> Inbound;
    std::vector<TDagNode*> Outbound;
    IRuntimeNode* Compute;
};

struct TGraph {
    std::unordered_map<IRuntimeNode*, std::vector<IRuntimeNode*>> Inbound;
    std::unordered_map<IRuntimeNode*, std::vector<IRuntimeNode*>> Outbound;
    std::vector<std::unique_ptr<IConnection>> Connections;

    std::vector<std::unique_ptr<TDagNode>> DagNodes;
    std::unordered_map<IRuntimeNode*, TDagNode*> NodeToDagNode;
    TDagNode* Root = nullptr;

    void Build() {
        for (auto& [dst, _] : Inbound) {
            auto dagNode = std::make_unique<TDagNode>();
            NodeToDagNode[dst] = dagNode.get();
            dagNode->Compute = dst;

            if (!Outbound.contains(dst)) {
                Root = dagNode.get();
            }

            DagNodes.emplace_back(std::move(dagNode));
        }

        for (auto& [src, _] : Outbound) {
            if (NodeToDagNode.contains(src)) {
                continue;
            }

            auto dagNode = std::make_unique<TDagNode>();
            dagNode->Compute = src;
            NodeToDagNode[src] = dagNode.get();
            DagNodes.emplace_back(std::move(dagNode));
        }

        for (auto& [dst, srcs] : Inbound) {
            auto node = NodeToDagNode[dst];
            for (auto src : srcs) {
                auto srcNode = NodeToDagNode[src];
                assert(srcNode);
                node->Inbound.emplace_back(srcNode);
            }
        }

        for (auto& [src, dsts] : Outbound) {
            auto node = NodeToDagNode[src];
            for (auto dst : dsts) {
                auto dstNode = NodeToDagNode[dst];
                node->Outbound.emplace_back(dstNode);
            }
        }
    }

    void Connect(IRuntimeNode* src, IRuntimeNode* dst, EConnectionKind kind)
    {
        Inbound[dst].push_back(src);
        Outbound[src].push_back(dst);

        switch (kind) {
        case EConnectionKind::OneToOne: {
            auto conn = std::make_unique<TOneToOne>();
            src->SetOutputPort(TOutputPort(conn.get(), 0));
            dst->SetInputPort(TInputPort(conn.get(), 0));
            Connections.emplace_back(std::move(conn));
            break;
        }
        default:
            assert(false);
            break;
        };
    }
};

struct TNode : virtual IRuntimeNode {
    TInputPort InputPort_;
    TOutputPort OutputPort_;

    // connection
    void SetInputPort(TInputPort inputPort) override {
        InputPort_ = std::move(inputPort);
    }

    void SetOutputPort(TOutputPort outputPort) override {
        OutputPort_ = std::move(outputPort);
    }
};

struct TNodeWithInput : TNode {
    TNodeWithInput()
    { }

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

    bool InputConsumed_ = false;
    std::optional<TRowSet> CurrentInput_;
};

struct TScan : TNode {
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

struct TFilter : TNodeWithInput {
    int Seed;
    int Percent;

    TFilter(int seed, int percent)
        : Seed(seed)
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

struct TPrinter : TNodeWithInput {
    TPrinter()
    { }

    void Run(TRowSet& rowSet) override {
        for (int i = 0; i < rowSet.Col.size(); ++i) {
            if (rowSet.Selection[i]) {
                std::cout << rowSet.Col[i] << "\n";
            }
        }
    }
};

struct TNullScheduler {
    bool Step(TDagNode* node, TRowSet& rowSet) {
        if (node->Inbound.empty()) {
            if (node->Compute->IsFinished()) {
                return false;
            }

            node->Compute->Run(rowSet);
            return true;
        }

        // assume have only 1 input node
        if (!Step(node->Inbound.front(), rowSet)) {
            return false;
        }

        node->Compute->Run(rowSet);
        return true;
    }

    void Run(TDagNode* root) {
        TRowSet rowSet;
        while (Step(root, rowSet)) {
        }
    }
};

struct TScheduler {
    TGraph& Graph;
    std::list<TDagNode*> Ready;
    std::unordered_set<TDagNode*> Scheduled;

    TScheduler(TGraph& g)
        : Graph(g)
    {
        Schedule(g.Root);
    }

    void Schedule(TDagNode* node) {
        if (!Scheduled.contains(node)) {
            Ready.emplace_back(node);
            Scheduled.emplace(node);
        }
    }

    void ScheduleInput(TDagNode* node) {
        for (auto in : node->Inbound) {
            Schedule(in);
        }
    }

    void ScheduleOutput(TDagNode* node) {
        for (auto out : node->Outbound) {
            Schedule(out);
        }
    }

    void Run() {
        while (!Ready.empty()) {
            auto node = Ready.front(); Ready.pop_front();
            Scheduled.erase(node);
            auto state = node->Compute->Execute();
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
    auto filter = std::make_unique<TFilter>(43, 30);
    auto printer = std::make_unique<TPrinter>();


    TGraph g;
    g.Connect(scan.get(), filter.get(), EConnectionKind::OneToOne);
    g.Connect(filter.get(), printer.get(), EConnectionKind::OneToOne);
    g.Build();

    TNullScheduler scheduler;
    scheduler.Run(g.Root);
}

TEST(Scheduler, Async) {
    int inputSets = 16;
    auto scan = std::make_unique<TScan>(inputSets, 42);
    auto filter = std::make_unique<TFilter>(43, 30);
    auto printer = std::make_unique<TPrinter>();

    TGraph g;
    g.Connect(scan.get(), filter.get(), EConnectionKind::OneToOne);
    g.Connect(filter.get(), printer.get(), EConnectionKind::OneToOne);
    g.Build();

    TScheduler scheduler(g);
    scheduler.Run();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}