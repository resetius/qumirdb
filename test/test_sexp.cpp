#include <gtest/gtest.h>

#include <qdb/ops/aggregate.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/project.h>
#include <qdb/ops/source.h>
#include <qdb/sexp/parser.h>
#include <qdb/sexp/printer.h>

#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <sstream>

using namespace NQqb;
using namespace NQqb::NSexp;
using namespace NQumir::NAst::NCore;
using namespace NQumir::NAst;

namespace {

struct TStubSource : ISource {
    std::vector<std::string> Names;
    std::vector<TColumnSchema> Cols;
    TSchema Schema_;

    explicit TStubSource(std::vector<std::string> colNames) {
        Names = std::move(colNames);
        for (auto& name : Names) {
            Cols.push_back({name, std::make_shared<TIntegerType>(TIntegerType::I64)});
        }
        Schema_ = TSchema{Cols};
    }

    const TSchema& Schema() const override { return Schema_; }
    bool Next(TRowSet&) override { return false; }
};

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
    TStubSource src({"x", "y"});
    auto op = std::make_shared<TSourceOperator>(src);
    EXPECT_EQ(PrintAst(op, MakePrintOpts()), "(rel source)");
}

TEST(SexpPrinter, Filter) {
    TStubSource src({"x"});
    auto source = std::make_shared<TSourceOperator>(src);
    auto filt = MakeFilter(source, "(> x 5)");
    ASSERT_TRUE(filt.has_value());
    EXPECT_EQ(PrintAst(*filt, MakePrintOpts()), "(rel filter (rel source) (> x 5))");
}

TEST(SexpPrinter, Project) {
    TStubSource src({"x", "y"});
    auto source = std::make_shared<TSourceOperator>(src);
    auto proj = MakeProject(source, {{"a", "x"}, {"b", "y"}});
    ASSERT_TRUE(proj.has_value());
    EXPECT_EQ(PrintAst(*proj, MakePrintOpts()), "(rel project (rel source) (a x) (b y))");
}

TEST(SexpPrinter, Aggregate) {
    TStubSource src({"x", "y"});
    auto source = std::make_shared<TSourceOperator>(src);
    auto agg = MakeAggregate(source, {"x"}, {{"cnt", "count", ""}, {"s", "sum", "y"}});
    ASSERT_TRUE(agg.has_value());
    EXPECT_EQ(PrintAst(*agg, MakePrintOpts()),
              "(rel aggregate (rel source) (keys x) (agg cnt count) (agg s sum y))");
}

TEST(SexpPrinter, FilterOfProject) {
    TStubSource src({"x", "y"});
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
    TStubSource src({"x"});
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
    TStubSource src({"x", "y"});
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
    TStubSource src({"x"});
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
    TStubSource src({"x", "y"});
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
