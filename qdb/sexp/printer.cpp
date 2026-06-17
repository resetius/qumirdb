#include <qdb/sexp/printer.h>

#include <qdb/ops/aggregate.h>
#include <qdb/ops/filter.h>
#include <qdb/ops/join.h>
#include <qdb/ops/operator.h>
#include <qdb/ops/project.h>
#include <qdb/ops/source.h>

#include <stdexcept>

namespace NQqb {
namespace NSexp {

using namespace NQumir::NAst::NCore;

static void PrintRel(NQumir::NAst::TExpr& expr, TPrinter& printer, TPrintFrame frame) {
    auto& op = static_cast<IOperator&>(expr);
    auto& out = printer.GetOut();
    const auto rel = op.RelName();

    if (rel == TSourceOperator::OpId) {
        auto& src = static_cast<TSourceOperator&>(op);
        out << "(rel source";
        if (!src.SourcePath().empty()) {
            out << ' ';
            printer.PrintString(src.SourcePath(), '"');
        }
        if (!src.GetAlias().empty()) {
            out << ' ';
            printer.PrintString(src.GetAlias(), '"');
        }
        out << ')';
        return;
    }

    if (rel == TFilterOperator::OpId) {
        auto& filt = static_cast<TFilterOperator&>(op);
        out << "(rel filter";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(filt.Input(), frame.AllowTypeWrap, frame.Level + 1);
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(filt.Predicate(), frame.AllowTypeWrap, frame.Level + 1);
        out << ')';
        return;
    }

    if (rel == TProjectOperator::OpId) {
        auto& proj = static_cast<TProjectOperator&>(op);
        out << "(rel project";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(proj.Input(), frame.AllowTypeWrap, frame.Level + 1);
        for (const auto& spec : proj.Projections()) {
            printer.Separator(frame.Level + 1);
            out << '(';
            printer.PrintIdentifier(spec.Name);
            printer.Space();
            printer.PrintExpr(spec.Expression, frame.AllowTypeWrap, frame.Level + 2);
            out << ')';
        }
        out << ')';
        return;
    }

    if (rel == TAggregateOperator::OpId) {
        auto& agg = static_cast<TAggregateOperator&>(op);
        out << "(rel aggregate";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(agg.Input(), frame.AllowTypeWrap, frame.Level + 1);
        printer.Separator(frame.Level + 1);
        out << "(keys";
        for (const auto& key : agg.GroupKeys()) {
            printer.Space();
            printer.PrintIdentifier(key);
        }
        out << ')';
        for (const auto& spec : agg.Aggs()) {
            printer.Separator(frame.Level + 1);
            out << "(agg ";
            printer.PrintIdentifier(spec.Name);
            printer.Space();
            printer.PrintIdentifier(spec.Func);
            if (spec.Arg) {
                printer.Space();
                printer.PrintExpr(spec.Arg, frame.AllowTypeWrap, frame.Level + 2);
            }
            out << ')';
        }
        out << ')';
        return;
    }

    if (rel == TJoinOperator::OpId) {
        auto& join = static_cast<TJoinOperator&>(op);
        out << "(rel join";
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(join.Left(), frame.AllowTypeWrap, frame.Level + 1);
        printer.Separator(frame.Level + 1);
        printer.PrintExpr(join.Right(), frame.AllowTypeWrap, frame.Level + 1);
        // Key list: ((lk rk) (lk rk) ...)
        printer.Separator(frame.Level + 1);
        out << '(';
        bool firstKey = true;
        for (const auto& key : join.Keys()) {
            if (!firstKey) {
                printer.Space();
            }
            firstKey = false;
            out << '(';
            printer.PrintIdentifier(key.Left);
            printer.Space();
            printer.PrintIdentifier(key.Right);
            out << ')';
        }
        out << ')';
        // Join type as a bare keyword: (inner)
        printer.Separator(frame.Level + 1);
        out << '(' << JoinTypeName(join.JoinType()) << ')';
        // Optional residual predicate, printed directly (no 'filter' label).
        if (join.Filter()) {
            printer.Separator(frame.Level + 1);
            printer.PrintExpr(join.Filter(), frame.AllowTypeWrap, frame.Level + 1);
        }
        out << ')';
        return;
    }

    throw std::runtime_error("PrintRel: unknown rel operator: " + std::string(rel));
}

TPrintExprFactory MakeRelPrinters() {
    return {{IOperator::NodeId, PrintRel}};
}

} // namespace NSexp
} // namespace NQqb
