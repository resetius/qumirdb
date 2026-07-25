#include <gtest/gtest.h>
#include "mock_source.h"

#include <qdb/plan/ops/aggregate.h>
#include <qdb/plan/ops/filter.h>
#include <qdb/plan/ops/join.h>
#include <qdb/plan/ops/limit.h>
#include <qdb/plan/ops/project.h>
#include <qdb/plan/ops/sort.h>
#include <qdb/plan/ops/source.h>
#include <qdb/plan/ops/window.h>
#include <qdb/sexp/parser.h>
#include <qdb/sexp/printer.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <sstream>

using namespace NQdb;
using namespace NQdb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {


TPrintOptions MakePrintOpts() {
    TPrintOptions opts;
    opts.Pretty = false;
    for (auto& [k, v] : MakeRelPrinters()) {
        opts.NodePrinters[k] = std::move(v);
    }
    return opts;
}

TParser MakeParser(TRelParserOptions opts = {}) {
    TParser p;
    for (auto& [k, v] : MakeRelParsers(std::move(opts))) {
        p.NodeParsers[k] = std::move(v);
    }
    return p;
}

TExprPtr Parse(TParser& p, const std::string& src) {
    std::istringstream in(src);
    TTokenStream ts(in);
    auto result = p.Parse(ts);
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().ToString());
    return result.value_or(nullptr);
}

} // namespace

// --- Printer ---

TEST(SexpPrinter, Source) {
    NQdb::TMockSource src({"x", "y"});
    auto op = std::make_shared<TSourceOperator>(src);
    EXPECT_EQ(PrintAst(op, MakePrintOpts()), "(rel source)");
}

TEST(SexpPrinter, Filter) {
    NQdb::TMockSource src({"x"});
    auto source = std::make_shared<TSourceOperator>(src);
    auto filt = MakeFilter(source, "(> x 5)");
    ASSERT_TRUE(filt.has_value());
    EXPECT_EQ(PrintAst(*filt, MakePrintOpts()), "(rel filter (rel source) (> x 5))");
}

TEST(SexpPrinter, Project) {
    NQdb::TMockSource src({"x", "y"});
    auto source = std::make_shared<TSourceOperator>(src);
    auto proj = MakeProject(source, {{"a", "x"}, {"b", "y"}});
    ASSERT_TRUE(proj.has_value());
    EXPECT_EQ(PrintAst(*proj, MakePrintOpts()), "(rel project (rel source) (a x) (b y))");
}

TEST(SexpPrinter, Aggregate) {
    NQdb::TMockSource src({"x", "y"});
    auto source = std::make_shared<TSourceOperator>(src);
    auto agg = MakeAggregate(source, {"x"}, {{"cnt", "count", ""}, {"s", "sum", "y"}});
    ASSERT_TRUE(agg.has_value());
    EXPECT_EQ(PrintAst(*agg, MakePrintOpts()),
              "(rel aggregate (rel source) (keys x) (agg cnt count) (agg s sum y))");
}

TEST(SexpPrinter, FilterOfProject) {
    NQdb::TMockSource src({"x", "y"});
    auto source = std::make_shared<TSourceOperator>(src);
    auto proj = MakeProject(source, {{"a", "x"}});
    ASSERT_TRUE(proj.has_value());
    auto filt = MakeFilter(*proj, "(> a 0)");
    ASSERT_TRUE(filt.has_value());
    EXPECT_EQ(PrintAst(*filt, MakePrintOpts()),
              "(rel filter (rel project (rel source) (a x)) (> a 0))");
}

// --- Parser ---

TEST(SexpParser, SourceNoFactory) {
    auto p = MakeParser();
    std::istringstream in("(rel source)");
    TTokenStream ts(in);
    EXPECT_FALSE(p.Parse(ts).has_value());
}

TEST(SexpParser, Filter) {
    NQdb::TMockSource src({"x"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
    sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
    return sourceOp;
};
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, "(rel filter (rel source \"data.parquet\") (> x 5))");
    ASSERT_NE(expr, nullptr);

    EXPECT_EQ(expr->NodeName(), IOperator::NodeId);
    EXPECT_EQ(static_cast<IOperator&>(*expr).RelName(), TFilterOperator::OpId);

    auto& filt = static_cast<TFilterOperator&>(*expr);
    EXPECT_EQ(filt.Input(), sourceOp);
    EXPECT_NE(filt.Predicate(), nullptr);
}

TEST(SexpParser, Project) {
    NQdb::TMockSource src({"x", "y"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
    sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
    return sourceOp;
};
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, "(rel project (rel source \"data.parquet\") (a x) (b y))");
    ASSERT_NE(expr, nullptr);

    auto& proj = static_cast<TProjectOperator&>(*expr);
    ASSERT_EQ(proj.Projections().size(), 2u);
    EXPECT_EQ(proj.Projections()[0].Name, "a");
    EXPECT_EQ(proj.Projections()[1].Name, "b");
}

TEST(SexpParser, FilterPrintRoundtrip) {
    NQdb::TMockSource src({"x"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);

    const std::string input = "(rel filter (rel source \"data.parquet\") (> x 5))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
    sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
    return sourceOp;
};
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, AggregatePrintRoundtrip) {
    NQdb::TMockSource src({"x", "y"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);

    const std::string input = "(rel aggregate (rel source \"data.parquet\") (keys x) (agg cnt count) (agg s sum y))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
    sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
    return sourceOp;
};
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);

    auto& agg = static_cast<TAggregateOperator&>(*expr);
    ASSERT_EQ(agg.GroupKeys().size(), 1u);
    EXPECT_EQ(agg.GroupKeys()[0], "x");
    ASSERT_EQ(agg.Aggs().size(), 2u);
    EXPECT_EQ(agg.Aggs()[0].Name, "cnt");
    EXPECT_EQ(agg.Aggs()[0].Func, "count");
    EXPECT_EQ(agg.Aggs()[0].Arg, nullptr);
    EXPECT_EQ(agg.Aggs()[1].Name, "s");
    EXPECT_EQ(agg.Aggs()[1].Func, "sum");
    EXPECT_NE(agg.Aggs()[1].Arg, nullptr);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, WindowPrintRoundtrip) {
    NQdb::TMockSource src({"a", "b", "c", "y"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);

    const std::string input =
        "(rel window (rel source \"data.parquet\") (partition a b) "
        "(order (c desc nulls-default)) "
        "(frame range (start unbounded-preceding) (end current-row)) "
        "(fn w_0 rank) (fn w_1 sum y))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
    sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
    return sourceOp;
};
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);
    ASSERT_EQ(static_cast<IOperator&>(*expr).RelName(), TWindowOperator::OpId);

    auto& window = static_cast<TWindowOperator&>(*expr);
    ASSERT_EQ(window.PartitionKeys().size(), 2u);
    EXPECT_EQ(window.PartitionKeys()[0], "a");
    EXPECT_EQ(window.PartitionKeys()[1], "b");
    ASSERT_EQ(window.OrderKeys().size(), 1u);
    EXPECT_EQ(window.OrderKeys()[0].Column, "c");
    EXPECT_EQ(window.OrderKeys()[0].Direction, ESortDirection::Desc);
    ASSERT_TRUE(window.Frame().has_value());
    EXPECT_EQ(window.Frame()->Mode, EWindowFrameMode::Range);
    EXPECT_EQ(window.Frame()->Start.Kind, EFrameBoundKind::UnboundedPreceding);
    EXPECT_EQ(window.Frame()->End.Kind, EFrameBoundKind::CurrentRow);
    ASSERT_EQ(window.Functions().size(), 2u);
    EXPECT_EQ(window.Functions()[0].Func, "rank");
    EXPECT_EQ(window.Functions()[0].Arg, nullptr);
    EXPECT_EQ(window.Functions()[1].Func, "sum");
    EXPECT_NE(window.Functions()[1].Arg, nullptr);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, WindowNoFrameRoundtrip) {
    NQdb::TMockSource src({"a"});
    auto sourceOp = std::make_shared<TSourceOperator>(src);

    const std::string input = "(rel window (rel source \"data.parquet\") (fn w_0 rank))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
    sourceOp = std::make_shared<TSourceOperator>(src, std::string(path));
    return sourceOp;
};
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);
    auto& window = static_cast<TWindowOperator&>(*expr);
    EXPECT_TRUE(window.PartitionKeys().empty());
    EXPECT_TRUE(window.OrderKeys().empty());
    EXPECT_FALSE(window.Frame().has_value());

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, SortPrintRoundtrip) {
    NQdb::TMockSource src({"x", "y"});
    const std::string input =
        "(rel sort (rel source \"data.parquet\") (x desc nulls-default) (y asc nulls-last))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);

    auto& sort = static_cast<TSortOperator&>(*expr);
    ASSERT_EQ(sort.Keys().size(), 2u);
    EXPECT_EQ(sort.Keys()[0].Column, "x");
    EXPECT_EQ(sort.Keys()[0].Direction, ESortDirection::Desc);
    EXPECT_EQ(sort.Keys()[1].Nulls, ESortNulls::Last);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, TopSortPrintRoundtrip) {
    NQdb::TMockSource src({"x"});
    const std::string input =
        "(rel top-sort (rel source \"data.parquet\") (x desc nulls-default) (limit 10))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);

    auto& sort = static_cast<TTopSortOperator&>(*expr);
    ASSERT_EQ(sort.Keys().size(), 1u);
    EXPECT_EQ(sort.Keys()[0].Column, "x");
    EXPECT_EQ(sort.Limit(), 10);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, LimitPrintRoundtrip) {
    NQdb::TMockSource src({"x"});
    const std::string input =
        "(rel limit (rel source \"data.parquet\") (limit 10) (offset 2))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        return std::make_shared<TSourceOperator>(src, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);

    auto& limit = static_cast<TLimitOperator&>(*expr);
    EXPECT_EQ(limit.Limit(), 10);
    EXPECT_EQ(limit.Offset(), 2);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpPrinter, Join) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});
    auto l = std::make_shared<TSourceOperator>(leftSrc);
    auto r = std::make_shared<TSourceOperator>(rightSrc);
    auto join = MakeJoin(l, r, {{"a", "c"}}, EJoinType::Inner);
    ASSERT_TRUE(join.has_value()) << (join ? "" : join.error().ToString());
    EXPECT_EQ(PrintAst(*join, MakePrintOpts()),
              "(rel join (rel source) (rel source) ((a c)) (inner))");
}

TEST(SexpPrinter, JoinWithFilterAndMultipleKeys) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});
    auto l = std::make_shared<TSourceOperator>(leftSrc);
    auto r = std::make_shared<TSourceOperator>(rightSrc);
    auto join = MakeJoin(l, r, {{"a", "c"}, {"b", "d"}}, EJoinType::Left, "(< b d)");
    ASSERT_TRUE(join.has_value()) << (join ? "" : join.error().ToString());
    EXPECT_EQ(PrintAst(*join, MakePrintOpts()),
              "(rel join (rel source) (rel source) ((a c) (b d)) (left) (< b d))");
}

TEST(SexpParser, JoinPrintRoundtrip) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});

    const std::string input =
        "(rel join (rel source \"left.parquet\") (rel source \"right.parquet\") "
        "((a c)) (left) (< b d))";

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        if (path == "left.parquet") {
            return std::make_shared<TSourceOperator>(leftSrc, std::string(path));
        }
        return std::make_shared<TSourceOperator>(rightSrc, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    auto expr = Parse(p, input);
    ASSERT_NE(expr, nullptr);

    auto& join = static_cast<TJoinOperator&>(*expr);
    ASSERT_EQ(join.Keys().size(), 1u);
    EXPECT_EQ(join.Keys()[0].Left, "a");
    EXPECT_EQ(join.Keys()[0].Right, "c");
    EXPECT_EQ(join.JoinType(), EJoinType::Left);
    EXPECT_NE(join.Filter(), nullptr);

    auto printed = PrintAst(std::static_pointer_cast<TExpr>(expr), MakePrintOpts());
    EXPECT_EQ(printed, input);
}

TEST(SexpParser, CrossJoinAcceptsEmptyKeyList) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        if (path == "left.parquet") {
            return std::make_shared<TSourceOperator>(leftSrc, std::string(path));
        }
        return std::make_shared<TSourceOperator>(rightSrc, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    // Empty key list + inner → cross join, must parse successfully.
    std::istringstream in(
        "(rel join (rel source \"left.parquet\") (rel source \"right.parquet\") () (inner))");
    TTokenStream ts(in);
    EXPECT_TRUE(p.Parse(ts).has_value());
}

TEST(SexpParser, JoinRejectsEmptyKeysWithNonInnerType) {
    NQdb::TMockSource leftSrc({"a", "b"});
    NQdb::TMockSource rightSrc({"c", "d"});

    TRelParserOptions opts;
    opts.SourceFactory = [&](std::string_view path, NQumir::TLocation) -> TOperatorPtr {
        if (path == "left.parquet") {
            return std::make_shared<TSourceOperator>(leftSrc, std::string(path));
        }
        return std::make_shared<TSourceOperator>(rightSrc, std::string(path));
    };
    auto p = MakeParser(std::move(opts));

    std::istringstream in(
        "(rel join (rel source \"left.parquet\") (rel source \"right.parquet\") () (left_semi))");
    TTokenStream ts(in);
    EXPECT_FALSE(p.Parse(ts).has_value());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
