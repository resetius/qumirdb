#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/io/io.h>
#include <qdb/io/schema.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/equijoin.h>
#include <qdb/plan/passes/join_order.h>
#include <qdb/plan/passes/qualify_columns.h>
#include <qdb/plan/passes/typing.h>
#include <qdb/sexp/parser.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/type.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace NQdb;
using namespace NQdb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {

TOperatorPtr BuildAnnotated(const std::string& sexp, const std::map<std::string, ISource*>& tables) {
    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(*tables.at(std::string(path)), std::string(path));
    };

    TParser parser;
    for (auto& [name, fn] : MakeRelParsers(std::move(opts))) {
        parser.NodeParsers[name] = std::move(fn);
    }

    std::istringstream in(sexp);
    TTokenStream ts(in);
    auto parsed = parser.Parse(ts);
    if (!parsed) {
        throw std::runtime_error(parsed.error().ToString());
    }

    auto root = std::static_pointer_cast<IOperator>(*parsed);
    AssignSourceAliases(root);
    QualifyColumns(root);
    AnnotateTypes(root);
    return root;
}

TOperatorPtr Optimize(const std::string& sexp, const std::map<std::string, ISource*>& tables) {
    return ExtractEquiJoins(BuildAnnotated(sexp, tables));
}

void CollectKeys(const TOperatorPtr& op, std::vector<std::pair<std::string, std::string>>& out) {
    if (auto join = TMaybeOp<TJoinOperator>(op)) {
        for (const auto& key : join.Cast()->Keys()) {
            out.emplace_back(key.Left, key.Right);
        }
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = TMaybeNode<IOperator>(child)) {
            CollectKeys(childOp.Cast(), out);
        }
    }
}

std::vector<std::pair<std::string, std::string>> Keys(const TOperatorPtr& root) {
    std::vector<std::pair<std::string, std::string>> out;
    CollectKeys(root, out);
    std::sort(out.begin(), out.end());
    return out;
}

using TKeys = std::vector<std::pair<std::string, std::string>>;

const TSourceOperator* AsSource(const TOperatorPtr& op) {
    return dynamic_cast<const TSourceOperator*>(op.get());
}

const TJoinOperator* AsJoin(const TOperatorPtr& op) {
    return dynamic_cast<const TJoinOperator*>(op.get());
}

const TFilterOperator* AsFilter(const TOperatorPtr& op) {
    return dynamic_cast<const TFilterOperator*>(op.get());
}

const TSortOperator* AsSort(const TOperatorPtr& op) {
    return dynamic_cast<const TSortOperator*>(op.get());
}

const TLimitOperator* AsLimit(const TOperatorPtr& op) {
    return dynamic_cast<const TLimitOperator*>(op.get());
}

std::set<std::string> LeafAliases(const TOperatorPtr& op) {
    std::set<std::string> out;
    if (auto* source = AsSource(op)) {
        out.insert(std::string(source->GetAlias()));
        return out;
    }
    for (const auto& child : op->Children()) {
        if (auto childOp = TMaybeNode<IOperator>(child)) {
            out.merge(LeafAliases(childOp.Cast()));
        }
    }
    return out;
}

} // namespace

// one join: filter (== aid bid) over cross(A, B) -> key (a.aid, b.bid)
TEST(EquiJoin, OneJoinFromFilter) {
    NQdb::TMockSource a({"aid", "aval"});
    NQdb::TMockSource b({"bid", "bval"});
    std::map<std::string, ISource*> tables = {{"A", &a}, {"B", &b}};

    auto root = Optimize(
        "(rel filter"
        "  (rel join (rel source \"A\" \"a\") (rel source \"B\" \"b\") () (inner))"
        "  (== aid bid))",
        tables);

    EXPECT_EQ(Keys(root), (TKeys{{"a.aid", "b.bid"}}));
}

// two joins: ((A x B) x C) with aid=bid and bcid=cid
TEST(EquiJoin, TwoJoinsFromFilter) {
    NQdb::TMockSource a({"aid", "aval"});
    NQdb::TMockSource b({"bid", "bcid", "bval"});
    NQdb::TMockSource c({"cid", "cval"});
    std::map<std::string, ISource*> tables = {{"A", &a}, {"B", &b}, {"C", &c}};

    auto root = Optimize(
        "(rel filter"
        "  (rel join"
        "    (rel join (rel source \"A\" \"a\") (rel source \"B\" \"b\") () (inner))"
        "    (rel source \"C\" \"c\") () (inner))"
        "  (&& (== aid bid) (== bcid cid)))",
        tables);

    EXPECT_EQ(Keys(root), (TKeys{{"a.aid", "b.bid"}, {"b.bcid", "c.cid"}}));
}

// three joins: (((A x B) x C) x D) with aid=bid, bcid=cid, ccid=did
TEST(EquiJoin, ThreeJoinsFromFilter) {
    NQdb::TMockSource a({"aid", "aval"});
    NQdb::TMockSource b({"bid", "bcid", "bval"});
    NQdb::TMockSource c({"cid", "ccid", "cval"});
    NQdb::TMockSource d({"did", "dval"});
    std::map<std::string, ISource*> tables = {{"A", &a}, {"B", &b}, {"C", &c}, {"D", &d}};

    auto root = Optimize(
        "(rel filter"
        "  (rel join"
        "    (rel join"
        "      (rel join (rel source \"A\" \"a\") (rel source \"B\" \"b\") () (inner))"
        "      (rel source \"C\" \"c\") () (inner))"
        "    (rel source \"D\" \"d\") () (inner))"
        "  (&& (&& (== aid bid) (== bcid cid)) (== ccid did)))",
        tables);

    EXPECT_EQ(Keys(root), (TKeys{{"a.aid", "b.bid"}, {"b.bcid", "c.cid"}, {"c.ccid", "d.did"}}));
}

TEST(EquiJoin, ExtractsKeysThroughLimitAndSort) {
    NQdb::TMockSource a({"aid", "aval"});
    NQdb::TMockSource b({"bid", "bval"});

    auto left = std::make_shared<TSourceOperator>(a, "A");
    left->SetAlias("a");
    auto right = std::make_shared<TSourceOperator>(b, "B");
    right->SetAlias("b");

    TOperatorPtr root = std::make_shared<TLimitOperator>(
        std::make_shared<TSortOperator>(
            std::make_shared<TFilterOperator>(
                std::make_shared<TJoinOperator>(
                    left, right, std::vector<TJoinKey>{}, EJoinType::Inner, nullptr),
                std::make_shared<TBinaryExpr>(
                    NQumir::TLocation{}, TOperator("=="),
                    std::make_shared<TIdentExpr>(NQumir::TLocation{}, "aid"),
                    std::make_shared<TIdentExpr>(NQumir::TLocation{}, "bid"))),
            std::vector<TSortKey>{{"aval", ESortDirection::Asc, ESortNulls::Default}}),
        10);

    QualifyColumns(root);
    AnnotateTypes(root);
    root = ExtractEquiJoins(root);

    EXPECT_EQ(Keys(root), (TKeys{{"a.aid", "b.bid"}}));
}

TEST(JoinOrder, ReordersThroughLimitAndSort) {
    NQdb::TMockSource a({"aid", "aval"});
    NQdb::TMockSource b({"bid", "bval"});
    NQdb::TMockSource c({"cid", "cval"});

    auto sourceA = std::make_shared<TSourceOperator>(a, "A");
    sourceA->SetAlias("a");
    auto sourceB = std::make_shared<TSourceOperator>(b, "B");
    sourceB->SetAlias("b");
    auto sourceC = std::make_shared<TSourceOperator>(c, "C");
    sourceC->SetAlias("c");

    TOperatorPtr root = std::make_shared<TLimitOperator>(
        std::make_shared<TSortOperator>(
            std::make_shared<TFilterOperator>(
                std::make_shared<TJoinOperator>(
                    std::make_shared<TJoinOperator>(
                        sourceA, sourceC, std::vector<TJoinKey>{}, EJoinType::Inner, nullptr),
                    sourceB, std::vector<TJoinKey>{}, EJoinType::Inner, nullptr),
                std::make_shared<TBinaryExpr>(
                    NQumir::TLocation{}, TOperator("=="),
                    std::make_shared<TIdentExpr>(NQumir::TLocation{}, "aid"),
                    std::make_shared<TIdentExpr>(NQumir::TLocation{}, "bid"))),
            std::vector<TSortKey>{{"aval", ESortDirection::Asc, ESortNulls::Default}}),
        10);

    QualifyColumns(root);
    AnnotateTypes(root);
    root = ReorderJoins(root);

    auto limit = AsLimit(root);
    ASSERT_NE(limit, nullptr);
    auto sort = AsSort(limit->Input());
    ASSERT_NE(sort, nullptr);
    auto filter = AsFilter(sort->Input());
    ASSERT_NE(filter, nullptr);
    auto topJoin = AsJoin(filter->Input());
    ASSERT_NE(topJoin, nullptr);

    // Only aid=bid connects the graph, so {a,b} is one component joined together
    // and {c} is cross-joined against it (orientation is not fixed).
    auto left = LeafAliases(topJoin->Left());
    auto right = LeafAliases(topJoin->Right());
    const std::set<std::string> connected{"a", "b"};
    const std::set<std::string> crossed{"c"};
    EXPECT_TRUE((left == connected && right == crossed)
        || (left == crossed && right == connected)) << "unexpected join partition";
}

// PushDownPredicates moves the single-table predicate onto A's leaf, leaves the
// cross-table equality above the join, and keeps the join empty-key (reorderable).
TEST(PushDown, SingleTableToLeafEqualityStaysAbove) {
    NQdb::TMockSource a({"aid", "aval"});
    NQdb::TMockSource b({"bid", "bval"});
    std::map<std::string, ISource*> tables = {{"A", &a}, {"B", &b}};

    auto root = PushDownPredicates(BuildAnnotated(
        "(rel filter"
        "  (rel join (rel source \"A\" \"a\") (rel source \"B\" \"b\") () (inner))"
        "  (&& (== aid bid) (< aval 5)))",
        tables));

    // equality stays materialized above the join (edge source for ReorderJoins)
    auto topFilter = AsFilter(root);
    ASSERT_NE(topFilter, nullptr);

    // join is still an empty-key, residual-free inner join -> reorderable
    auto join = AsJoin(topFilter->Input());
    ASSERT_NE(join, nullptr);
    EXPECT_TRUE(join->Keys().empty());
    EXPECT_EQ(join->Filter(), nullptr);

    // aval<5 landed on A's leaf; B is left a bare source
    auto leftFilter = AsFilter(join->Left());
    ASSERT_NE(leftFilter, nullptr);
    ASSERT_NE(AsSource(leftFilter->Input()), nullptr);
    EXPECT_EQ(AsSource(leftFilter->Input())->GetAlias(), "a");
    ASSERT_NE(AsSource(join->Right()), nullptr);
    EXPECT_EQ(AsSource(join->Right())->GetAlias(), "b");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
