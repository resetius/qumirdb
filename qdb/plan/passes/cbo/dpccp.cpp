#include <qdb/plan/passes/cbo/dpccp.h>

#include <qdb/plan/ops/join.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace NQdb {
namespace NCbo {
namespace {

using TSubset = uint32_t; // bitmask over leaf indices

struct TGraphEdge {
    size_t A;
    size_t B;
    double Sel;
};

struct TDpEntry {
    TOperatorPtr Tree;
    double Cost = 0.0;
};

constexpr double DefaultNdv = 100.0;

double Ndv(const TOperatorPtr& leaf, const std::string& col) {
    const auto& stats = leaf->Stats_;
    if (!stats) {
        return DefaultNdv;
    }
    double cap = stats->RowCount > 0 ? static_cast<double>(stats->RowCount) : DefaultNdv;
    auto it = stats->ColumnStats.find(col);
    if (it == stats->ColumnStats.end() || !it->second || !it->second->Ndv) {
        return std::min(DefaultNdv, cap);
    }
    return std::min(static_cast<double>(*it->second->Ndv), cap);
}

// DP over subsets ordered by increasing bitmask, so every proper subset is
// solved before its supersets. Optimal bushy tree; DPccp's csg-cmp enumeration
// (see header link) is a drop-in optimization for large n.
class TDpccp {
public:
    TDpccp(const std::vector<TOperatorPtr>& leaves, const std::vector<TJoinEdge>& edges)
        : Leaves_(leaves)
        , N_(leaves.size())
        , Neighbors_(leaves.size(), 0)
    {
        BuildGraph(edges);
    }

    TOperatorPtr Solve() {
        const TSubset full = (TSubset{1} << N_) - 1;
        std::vector<double> card(full + 1, 0.0);
        std::vector<TDpEntry> dp(full + 1);

        for (size_t i = 0; i < N_; ++i) {
            const TSubset s = TSubset{1} << i;
            card[s] = Leaves_[i]->Stats_ ? static_cast<double>(Leaves_[i]->Stats_->RowCount) : 0.0;
            dp[s] = {Leaves_[i], 0.0};
        }

        for (TSubset s = 1; s <= full; ++s) {
            if (std::popcount(s) < 2 || !Connected(s)) {
                continue;
            }
            for (TSubset s1 = (s - 1) & s; s1; s1 = (s1 - 1) & s) {
                const TSubset s2 = s ^ s1;
                if (!dp[s1].Tree || !dp[s2].Tree) {
                    continue; // a side is disconnected
                }
                const double jc = card[s1] * card[s2] * CrossingSel(s1, s2);
                const double cost = dp[s1].Cost + dp[s2].Cost + jc;
                if (!dp[s].Tree || cost < dp[s].Cost) {
                    dp[s] = {MakeJoin(dp[s1].Tree, dp[s2].Tree), cost};
                    card[s] = jc;
                }
            }
        }
        return dp[full].Tree; // null when the graph is disconnected
    }

private:
    void BuildGraph(const std::vector<TJoinEdge>& edges) {
        std::unordered_map<std::string, size_t> owner;
        for (size_t i = 0; i < N_; ++i) {
            auto* schema = static_cast<NQumir::NAst::TStructType*>(Leaves_[i]->OutputColumns().get());
            if (!schema) {
                continue;
            }
            for (const auto& [name, _] : schema->Fields) {
                owner.emplace(name, i);
            }
        }
        for (const auto& e : edges) {
            auto la = owner.find(e.LeftCol);
            auto lb = owner.find(e.RightCol);
            if (la == owner.end() || lb == owner.end() || la->second == lb->second) {
                continue;
            }
            const size_t a = la->second;
            const size_t b = lb->second;
            const double denom = std::max(
                1.0, std::max(Ndv(Leaves_[a], e.LeftCol), Ndv(Leaves_[b], e.RightCol)));
            Edges_.push_back({a, b, 1.0 / denom});
            Neighbors_[a] |= TSubset{1} << b;
            Neighbors_[b] |= TSubset{1} << a;
        }
    }

    bool Connected(TSubset s) const {
        TSubset visited = s & (~s + 1); // lowest set bit
        TSubset frontier = visited;
        while (frontier) {
            TSubset next = 0;
            for (TSubset m = frontier; m; m &= m - 1) {
                next |= Neighbors_[std::countr_zero(m)];
            }
            next &= s & ~visited;
            visited |= next;
            frontier = next;
        }
        return visited == s;
    }

    double CrossingSel(TSubset s1, TSubset s2) const {
        double sel = 1.0;
        for (const auto& e : Edges_) {
            const TSubset a = TSubset{1} << e.A;
            const TSubset b = TSubset{1} << e.B;
            if (((a & s1) && (b & s2)) || ((a & s2) && (b & s1))) {
                sel *= e.Sel;
            }
        }
        return sel;
    }

    static TOperatorPtr MakeJoin(const TOperatorPtr& l, const TOperatorPtr& r) {
        return std::make_shared<TJoinOperator>(
            l, r, std::vector<TJoinKey>{}, EJoinType::Inner, nullptr);
    }

    const std::vector<TOperatorPtr>& Leaves_;
    size_t N_;
    std::vector<TSubset> Neighbors_;
    std::vector<TGraphEdge> Edges_;
};

} // namespace

TOperatorPtr DpccpJoinOrder(
    const std::vector<TOperatorPtr>& leaves,
    const std::vector<TJoinEdge>& edges)
{
    if (leaves.empty()) {
        return nullptr;
    }
    if (leaves.size() == 1) {
        return leaves.front();
    }
    if (leaves.size() > MaxRelations) {
        return nullptr;
    }
    return TDpccp(leaves, edges).Solve();
}

} // namespace NCbo
} // namespace NQdb
