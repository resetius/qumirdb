#include <gtest/gtest.h>

#include <qdb/io/io.h>
#include <qdb/io/schema.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/operator.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/passes/equijoin.h>
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
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace NQqb;
using namespace NQqb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {

// Schema-only mock; never executed (the equi-join pass is purely logical).
struct TMockSource : ISource {
    std::vector<std::string> Names;
    std::vector<TColumnSchema> Cols;
    TSchema Schema_;

    explicit TMockSource(std::vector<std::string> names)
        : Names(std::move(names))
    {
        Cols.reserve(Names.size());
        for (const auto& name : Names) {
            Cols.push_back({ .Name = name, .Type = std::make_shared<TIntegerType>(TIntegerType::I64) });
        }
        Schema_ = TSchema{ Cols };
    }

    const TSchema& Schema() const override { return Schema_; }
    bool Next(TRowSet&) override { return false; }
};

TOperatorPtr Optimize(const std::string& sexp, const std::map<std::string, ISource*>& tables) {
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
    return ExtractEquiJoins(root);
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

} // namespace

// one join: filter (== aid bid) over cross(A, B) -> key (a.aid, b.bid)
TEST(EquiJoin, OneJoinFromFilter) {
    TMockSource a({"aid", "aval"});
    TMockSource b({"bid", "bval"});
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
    TMockSource a({"aid", "aval"});
    TMockSource b({"bid", "bcid", "bval"});
    TMockSource c({"cid", "cval"});
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
    TMockSource a({"aid", "aval"});
    TMockSource b({"bid", "bcid", "bval"});
    TMockSource c({"cid", "ccid", "cval"});
    TMockSource d({"did", "dval"});
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
