#include <gtest/gtest.h>

#include <deque>
#include <vector>
#include <list>

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
    FINISHED = 2,
};

struct IRuntimeNode {
    virtual ~IRuntimeNode() = default;
    virtual bool Next(TRowSet& rowSet) = 0;

    virtual bool Fetch(TRowSet& rowSet) = 0;
    virtual EState Execute() = 0;

    IRuntimeNode* OutputNode_ = nullptr;
    IRuntimeNode* OutputNode() {
        return OutputNode_;
    }

    virtual IRuntimeNode* InputNode() {
        return nullptr;
    }
};

struct TScan : IRuntimeNode {
    bool NSets;
    int Seed;

    int Ncols = 16;

    int SetId = 0;

    std::deque<TRowSet> Output;

    TScan(int nsets, int seed)
        : NSets(nsets)
        , Seed(seed)
    { }

    EState Execute() {
        GenerateRowSet(Output.emplace_back(), Ncols, &Seed);
        SetId ++;
        return SetId == NSets ? EState::FINISHED : EState::OK;
    }

    bool Fetch(TRowSet& rowSet) {
        if (Output.empty() && SetId == NSets) {
            return false;
        }

        if (!Output.empty()) {
            rowSet = std::move(Output.front()); Output.pop_front();
        }

        return true;
    }

    bool Next(TRowSet& rowSet) {
        if (SetId == NSets) {
            return false;
        }

        GenerateRowSet(rowSet, Ncols, &Seed);
        SetId ++;

        return true;
    }
};

struct TFilter : IRuntimeNode {
    int Seed;
    int Percent;
    IRuntimeNode* Input;

    std::deque<TRowSet> InputRowSets;
    std::deque<TRowSet> Output;
    bool InputConsumed = false;

    TFilter(int seed, int percent, IRuntimeNode* input)
        : Seed(seed)
        , Percent(percent)
        , Input(input)
    {
        Input->OutputNode_ = this;
    }

    IRuntimeNode* InputNode() {
        return Input;
    }

    bool Fetch(TRowSet& rowSet) {
        if (Output.empty() && InputConsumed) {
            return false;
        }

        if (!Output.empty()) {
            rowSet = std::move(Output.front()); Output.pop_front();
        }

        return true;
    }

    EState Execute() {
        TRowSet rowSet;
        if (InputRowSets.empty()) {
            if (Input->Fetch(rowSet)) {
                if (!rowSet.Empty()) {
                    InputRowSets.emplace_back(std::move(rowSet));
                }
            } else {
                InputConsumed = true;
            }
        }

        if (InputRowSets.empty()) {
            return InputConsumed ? EState::FINISHED : EState::NEED_DATA;
        }

        for (auto&& input : InputRowSets) {
            Filter(input);
            Output.emplace_back(std::move(input));
        }
        InputRowSets.clear();

        return InputConsumed? EState::FINISHED : EState::OK;
    }

    void Filter(TRowSet& rowSet) {
        for (int i = 0; i < rowSet.Col.size(); ++i) {
            if (rowSet.Selection[i]) {
                int k = (RandomInt(&Seed) % 100);
                if (k >= Percent) {
                    rowSet.Selection[i] = 0;
                }
            }
        }
    }

    bool Next(TRowSet& rowSet) {
        if (!Input->Next(rowSet)) {
            return false;
        }

        Filter(rowSet);

        return true;
    }
};

struct TPrinter : IRuntimeNode {
    IRuntimeNode* Input;

    std::deque<TRowSet> InputRowSets;
    std::deque<TRowSet> Output;
    bool InputConsumed = false;

    TPrinter(IRuntimeNode* input)
        : Input(input)
    {
        Input->OutputNode_ = this;
    }

    IRuntimeNode* InputNode() {
        return Input;
    }

    bool Fetch(TRowSet& rowSet) {
        if (Output.empty() && InputConsumed) {
            return false;
        }

        if (!Output.empty()) {
            rowSet = std::move(Output.front()); Output.pop_front();
        }

        return true;
    }

    EState Execute() {
        TRowSet rowSet;
        if (InputRowSets.empty()) {
            if (Input->Fetch(rowSet)) {
                if (!rowSet.Empty()) {
                    InputRowSets.emplace_back(std::move(rowSet));
                }
            } else {
                InputConsumed = true;
            }
        }

        if (InputRowSets.empty()) {
            return InputConsumed ? EState::FINISHED : EState::NEED_DATA;
        }

        for (auto&& input : InputRowSets) {
            Print(input);
        }
        InputRowSets.clear();

        return InputConsumed ? EState::FINISHED : EState::OK;
    }

    void Print(TRowSet& rowSet) {
        for (int i = 0; i < rowSet.Col.size(); ++i) {
            if (rowSet.Selection[i]) {
                std::cout << rowSet.Col[i] << "\n";
            }
        }
    }

    bool Next(TRowSet& rowSet) {
        if (!Input->Next(rowSet)) {
            return false;
        }

        Print(rowSet);

        return true;
    }
};

struct TScheduler {
    std::list<IRuntimeNode*> Ready;

    void Schedule(IRuntimeNode* node) {
        if (node) {
            Ready.emplace_back(node);
        }
    }

    void Run() {
        while (!Ready.empty()) {
            auto node = Ready.front(); Ready.pop_front();
            auto state = node->Execute();
            if (state == EState::NEED_DATA) {
                Schedule(node->InputNode());
                Schedule(node);
            } else if (state == EState::OK) {
                Schedule(node);
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