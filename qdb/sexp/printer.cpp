#include <qdb/sexp/printer.h>

#include <qdb/ops/filter.h>
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

    throw std::runtime_error("PrintRel: unknown rel operator: " + std::string(rel));
}

TPrintExprFactory MakeRelPrinters() {
    return {{IOperator::NodeId, PrintRel}};
}

} // namespace NSexp
} // namespace NQqb
