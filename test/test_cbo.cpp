#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/stats.h>
#include <qdb/plan/passes/cbo/dpccp.h>
#include <qdb/plan/passes/estimate_stats.h>

#include <qumir/parser/type.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

using namespace NQdb;

namespace {

using TSubset = uint32_t;

struct TLeafSpec {
    uint64_t Rows;
    std::vector<std::pair<std::string, uint64_t>> Cols; // (column, ndv)
};

// Owns mock sources; builds leaf operators with Stats_ set.
struct TFixture {
    std::vector<std::unique_ptr<TMockSource>> Sources;
    std::vector<TOperatorPtr> Leaves;

    TOperatorPtr Add(const TLeafSpec& spec) {
        std::vector<std::string> names;
        for (const auto& col : spec.Cols) {
            names.push_back(col.first);
        }
        Sources.push_back(std::make_unique<TMockSource>(names));
        auto op = std::make_shared<TSourceOperator>(*Sources.back(), std::string{});
        auto stats = std::make_shared<TStats>();
        stats->RowCount = spec.Rows;
        for (const auto& [col, ndv] : spec.Cols) {
            auto cs = std::make_shared<TStats::TColumnStats>();
            cs->Ndv = ndv;
            stats->ColumnStats[col] = cs;
        }
        op->Stats_ = stats;
        Leaves.push_back(op);
        return op;
    }
};

// Independent Cout model — mirrors dpccp's cost, used to check optimality.
struct TModel {
    const std::vector<TOperatorPtr>& Leaves;
    std::map<const IOperator*, size_t> LeafIdx;
    std::vector<TSubset> Neighbors;
    std::vector<std::tuple<size_t, size_t, double>> Edges; // a, b, sel

    TModel(const std::vector<TOperatorPtr>& leaves, const std::vector<NCbo::TJoinEdge>& edges)
        : Leaves(leaves)
        , Neighbors(leaves.size(), 0)
    {
        std::map<std::string, size_t> owner;
        for (size_t i = 0; i < Leaves.size(); ++i) {
            LeafIdx[Leaves[i].get()] = i;
            auto* schema = static_cast<NQumir::NAst::TStructType*>(Leaves[i]->OutputColumns().get());
            for (const auto& [name, type] : schema->Fields) {
                owner[name] = i;
            }
        }
        std::map<std::pair<size_t, size_t>, std::vector<std::pair<double, double>>> pairKeys;
        for (const auto& e : edges) {
            size_t a = owner.at(e.LeftCol);
            size_t b = owner.at(e.RightCol);
            double na = Ndv(a, e.LeftCol);
            double nb = Ndv(b, e.RightCol);
            size_t i = std::min(a, b);
            size_t j = std::max(a, b);
            pairKeys[{i, j}].emplace_back(a == i ? na : nb, a == i ? nb : na);
            Neighbors[a] |= TSubset{1} << b;
            Neighbors[b] |= TSubset{1} << a;
        }
        for (const auto& [key, keys] : pairKeys) {
            double ri = LeafRows(key.first);
            double rj = LeafRows(key.second);
            double rows = EstimateEquiJoin(ri, rj, keys).Rows;
            double sel = ri * rj > 0.0 ? rows / (ri * rj) : 1.0;
            Edges.emplace_back(key.first, key.second, sel);
        }
    }

    double Ndv(size_t idx, const std::string& col) const {
        const auto& stats = Leaves[idx]->Stats_;
        auto it = stats->ColumnStats.find(col);
        return (it != stats->ColumnStats.end() && it->second->Ndv)
            ? static_cast<double>(*it->second->Ndv) : 100.0;
    }

    double LeafRows(size_t idx) const {
        return static_cast<double>(Leaves[idx]->Stats_->RowCount);
    }

    double CrossingSel(TSubset a, TSubset b) const {
        double sel = 1.0;
        for (const auto& [ea, eb, es] : Edges) {
            TSubset ma = TSubset{1} << ea;
            TSubset mb = TSubset{1} << eb;
            if (((ma & a) && (mb & b)) || ((ma & b) && (mb & a))) {
                sel *= es;
            }
        }
        return sel;
    }

    bool Connected(TSubset s) const {
        TSubset visited = s & (~s + 1);
        TSubset frontier = visited;
        while (frontier) {
            TSubset next = 0;
            for (TSubset m = frontier; m; m &= m - 1) {
                next |= Neighbors[std::countr_zero(m)];
            }
            next &= s & ~visited;
            visited |= next;
            frontier = next;
        }
        return visited == s;
    }

    struct TEval {
        TSubset Mask;
        double Card;
        double Cost;
    };

    // Cout of a concrete tree.
    TEval Eval(const TOperatorPtr& node) const {
        if (auto join = TMaybeOp<TJoinOperator>(node)) {
            auto left = Eval(join.Cast()->Left());
            auto right = Eval(join.Cast()->Right());
            double card = left.Card * right.Card * CrossingSel(left.Mask, right.Mask);
            return {left.Mask | right.Mask, card, left.Cost + right.Cost + card};
        }
        size_t idx = LeafIdx.at(node.get());
        return {TSubset{1} << idx, LeafRows(idx), 0.0};
    }

    // Brute-force optimum Cout over all bushy trees for the leaf set `s`.
    double BruteMin(TSubset s, std::vector<double>& cost, std::vector<double>& card) const {
        if (std::popcount(s) == 1) {
            card[s] = LeafRows(std::countr_zero(s));
            return cost[s] = 0.0;
        }
        if (cost[s] >= 0) {
            return cost[s];
        }
        double best = std::numeric_limits<double>::infinity();
        for (TSubset s1 = (s - 1) & s; s1; s1 = (s1 - 1) & s) {
            TSubset s2 = s ^ s1;
            if (!Connected(s1) || !Connected(s2)) {
                continue;
            }
            double c1 = BruteMin(s1, cost, card);
            double c2 = BruteMin(s2, cost, card);
            double jc = card[s1] * card[s2] * CrossingSel(s1, s2);
            if (c1 + c2 + jc < best) {
                best = c1 + c2 + jc;
                card[s] = jc;
            }
        }
        return cost[s] = best;
    }
};

// DPccp result must be an optimal-cost tree covering every leaf.
void ExpectOptimal(const TFixture& fx, const std::vector<NCbo::TJoinEdge>& edges) {
    auto tree = NCbo::DpccpJoinOrder(fx.Leaves, edges);
    ASSERT_NE(tree, nullptr);

    TModel model(fx.Leaves, edges);
    auto ev = model.Eval(tree);
    TSubset full = (TSubset{1} << fx.Leaves.size()) - 1;
    EXPECT_EQ(ev.Mask, full) << "tree does not cover all leaves";

    std::vector<double> cost(full + 1, -1.0);
    std::vector<double> card(full + 1, 0.0);
    double best = model.BruteMin(full, cost, card);
    EXPECT_NEAR(ev.Cost, best, 1e-6) << "DP tree is not optimal";
}

} // namespace

TEST(Cbo, ChainOptimal) {
    TFixture fx;
    fx.Add({1000, {{"a_id", 1000}}});
    fx.Add({500, {{"b_a", 500}, {"b_c", 50}}});
    fx.Add({20, {{"c_id", 20}}});
    fx.Add({100, {{"d_a", 100}}});
    // a-b, b-c, a-d
    ExpectOptimal(fx, {{"a_id", "b_a"}, {"b_c", "c_id"}, {"a_id", "d_a"}});
}

TEST(Cbo, StarOptimal) {
    TFixture fx;
    fx.Add({1000000, {{"f_d1", 1000000}, {"f_d2", 1000000}, {"f_d3", 1000000}}}); // fact
    fx.Add({10, {{"d1_id", 10}}});
    fx.Add({500, {{"d2_id", 500}}});
    fx.Add({50000, {{"d3_id", 50000}}});
    ExpectOptimal(fx, {{"f_d1", "d1_id"}, {"f_d2", "d2_id"}, {"f_d3", "d3_id"}});
}

TEST(Cbo, CycleOptimal) {
    TFixture fx;
    fx.Add({300, {{"a_id", 300}, {"a_d", 300}}});
    fx.Add({40, {{"b_a", 40}, {"b_c", 40}}});
    fx.Add({900, {{"c_id", 900}}});
    fx.Add({70, {{"d_id", 70}}});
    // a-b, b-c, c... actually cycle a-b, b-c, a-d, plus close: c-... use a-b,b-c,a-c,a-d
    ExpectOptimal(fx, {{"a_id", "b_a"}, {"b_c", "c_id"}, {"a_id", "c_id"}, {"a_d", "d_id"}});
}

TEST(Cbo, PkFkChainFiveOptimal) {
    TFixture fx;
    fx.Add({8000000, {{"ps_sk", 100000}, {"ps_pk", 200000}}});
    fx.Add({100000, {{"s_sk", 100000}, {"s_nk", 25}}});
    fx.Add({25, {{"n_nk", 25}, {"n_rk", 5}}});
    fx.Add({5, {{"r_rk", 5}}});
    fx.Add({200000, {{"p_pk", 200000}}});
    ExpectOptimal(fx, {
        {"ps_sk", "s_sk"}, {"s_nk", "n_nk"}, {"n_rk", "r_rk"}, {"ps_pk", "p_pk"}});
}

TEST(Cbo, DisconnectedReturnsNull) {
    TFixture fx;
    fx.Add({100, {{"a_id", 100}}});
    fx.Add({100, {{"b_id", 100}}}); // isolated
    fx.Add({100, {{"c_a", 100}}});
    EXPECT_EQ(NCbo::DpccpJoinOrder(fx.Leaves, {{"a_id", "c_a"}}), nullptr);
}

TEST(Cbo, SingleLeafReturnsLeaf) {
    TFixture fx;
    auto leaf = fx.Add({100, {{"a_id", 100}}});
    EXPECT_EQ(NCbo::DpccpJoinOrder(fx.Leaves, {}), leaf);
}

TEST(Cbo, FallbackAboveMaxRelations) {
    TFixture fx;
    for (size_t i = 0; i < NCbo::MaxRelations + 1; ++i) {
        fx.Add({100, {{"c" + std::to_string(i), 100}}});
    }
    EXPECT_EQ(NCbo::DpccpJoinOrder(fx.Leaves, {}), nullptr);
}

// Random connected graphs (spanning tree + extra edges) must all be optimal.
TEST(Cbo, RandomGraphsOptimal) {
    int seed = 987654321;
    auto rnd = [&](int lo, int hi) {
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        return lo + seed % (hi - lo + 1);
    };
    for (int iter = 0; iter < 100; ++iter) {
        size_t n = rnd(2, 6);
        std::vector<uint64_t> rows(n);
        std::vector<std::vector<std::pair<std::string, uint64_t>>> cols(n);
        for (size_t i = 0; i < n; ++i) {
            rows[i] = rnd(1, 100000);
        }
        std::vector<std::pair<size_t, size_t>> pairs;
        for (size_t i = 1; i < n; ++i) {                 // spanning tree -> connected
            pairs.emplace_back(i, static_cast<size_t>(rnd(0, static_cast<int>(i) - 1)));
        }
        for (int k = 0, extra = rnd(0, static_cast<int>(n)); k < extra; ++k) {
            size_t i = rnd(0, static_cast<int>(n) - 1);
            size_t j = rnd(0, static_cast<int>(n) - 1);
            if (i != j) {
                pairs.emplace_back(i, j);
            }
        }
        std::vector<NCbo::TJoinEdge> edges;
        for (size_t e = 0; e < pairs.size(); ++e) {
            auto [i, j] = pairs[e];
            std::string cl = "e" + std::to_string(e) + "l";
            std::string cr = "e" + std::to_string(e) + "r";
            cols[i].emplace_back(cl, rnd(1, static_cast<int>(rows[i])));
            cols[j].emplace_back(cr, rnd(1, static_cast<int>(rows[j])));
            edges.push_back({cl, cr});
        }
        TFixture fx;
        for (size_t i = 0; i < n; ++i) {
            if (cols[i].empty()) {
                cols[i].emplace_back("x" + std::to_string(i), rows[i]);
            }
            fx.Add({rows[i], cols[i]});
        }
        ExpectOptimal(fx, edges);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
