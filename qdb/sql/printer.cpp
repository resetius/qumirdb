#include "printer.h"

#include <qumir/parser/core/printer.h>

#include <memory>
#include <sstream>

namespace NQdb {
namespace NSql {

using NQumir::NAst::TExprPtr;
namespace NCore = NQumir::NAst::NCore;

namespace {

std::string KindName(TSubqueryExpr::EKind kind) {
    switch (kind) {
        case TSubqueryExpr::EKind::Scalar: return "scalar";
        case TSubqueryExpr::EKind::Exists: return "exists";
        case TSubqueryExpr::EKind::In:     return "in";
    }
    return "?";
}

std::string JoinTypeName(ESqlJoinType type) {
    switch (type) {
        case ESqlJoinType::Inner:     return "inner";
        case ESqlJoinType::Left:      return "left";
        case ESqlJoinType::Right:     return "right";
        case ESqlJoinType::LeftSemi:  return "left-semi";
        case ESqlJoinType::RightSemi: return "right-semi";
        case ESqlJoinType::Full:      return "full";
        case ESqlJoinType::Cross:     return "cross";
    }
    return "?";
}

std::string Join(const std::vector<std::string>& parts, char sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            out += sep;
        }
        out += parts[i];
    }
    return out;
}

struct TSqlPrinter {
    std::ostream& Out;

    void Indent(int n) {
        for (int i = 0; i < n; ++i) {
            Out << ' ';
        }
    }

    void Line(int ind, std::string_view s) {
        Indent(ind);
        Out << s << '\n';
    }

    NCore::TPrintOptions ExprOptions() {
        NCore::TPrintOptions opts;
        opts.NodePrinters[TSubqueryExpr::NodeId] =
            [](NQumir::NAst::TExpr& node, NCore::TPrinter& p, NCore::TPrintFrame frame) {
                auto& sub = static_cast<TSubqueryExpr&>(node);
                auto& out = p.GetOut();
                out << "(subquery " << KindName(sub.Kind);
                if (sub.Operand) {
                    out << ' ';
                    p.PrintExpr(sub.Operand, false, frame.Level);
                }
                out << ' ' << PrintAst(std::static_pointer_cast<TSqlNode>(sub.Query));
                out << ')';
            };
        return opts;
    }

    // delegate expressions to the Qumir core printer, re-indenting its output
    void Expr(int ind, const TExprPtr& e) {
        if (!e) {
            Line(ind, "(null)");
            return;
        }
        std::istringstream lines(NCore::PrintAst(e, ExprOptions()));
        std::string line;
        while (std::getline(lines, line)) {
            Indent(ind);
            Out << line << '\n';
        }
    }

    void IdentList(int ind, std::string_view head, const TSqlPtr<TIdentList>& list) {
        std::string s(head);
        if (list) {
            s += ' ';
            s += Join(list->Items, ' ');
        }
        s += ')';
        Line(ind, s);
    }

    void Query(int ind, const TSqlPtr<TSqlQuery>& q) {
        if (!q) {
            Line(ind, "(query null)");
            return;
        }
        Line(ind, "(query");
        if (q->WithClause) {
            With(ind + 2, q->WithClause);
        }
        Body(ind + 2, q->Body);
        if (q->OrderBy) {
            Order(ind + 2, q->OrderBy);
        }
        if (q->Limit) {
            Line(ind + 2, "(limit");
            Expr(ind + 4, q->Limit);
            Line(ind + 2, ")");
        }
        if (q->Offset) {
            Line(ind + 2, "(offset");
            Expr(ind + 4, q->Offset);
            Line(ind + 2, ")");
        }
        Line(ind, ")");
    }

    void With(int ind, const TSqlPtr<TSqlWithClause>& w) {
        Line(ind, w->Recursive ? "(with recursive" : "(with");
        for (const auto& cte : w->Ctes) {
            Cte(ind + 2, cte);
        }
        Line(ind, ")");
    }

    void Cte(int ind, const TSqlPtr<TSqlCte>& c) {
        std::string head = "(cte " + c->Name;
        if (c->Columns) {
            head += " cols: " + Join(c->Columns->Items, ' ');
        }
        Line(ind, head);
        Query(ind + 2, c->Query);
        Line(ind, ")");
    }

    void Body(int ind, const TSqlNodePtr& body) {
        if (auto select = std::dynamic_pointer_cast<TSqlSelect>(body)) {
            Select(ind, select);
            return;
        }
        Line(ind, "(unknown-body)");
    }

    void Select(int ind, const TSqlPtr<TSqlSelect>& s) {
        Line(ind, s->Quantifier == ESetQuantifier::Distinct ? "(select distinct" : "(select");
        if (s->SelectList) {
            SelectList(ind + 2, s->SelectList);
        }
        if (s->From) {
            From(ind + 2, s->From);
        }
        if (s->Where) {
            Line(ind + 2, "(where");
            Expr(ind + 4, s->Where);
            Line(ind + 2, ")");
        }
        if (s->GroupBy) {
            GroupBy(ind + 2, s->GroupBy);
        }
        if (s->Having) {
            Line(ind + 2, "(having");
            Expr(ind + 4, s->Having);
            Line(ind + 2, ")");
        }
        Line(ind, ")");
    }

    void SelectList(int ind, const TSqlPtr<TSqlSelectList>& list) {
        Line(ind, "(select-list");
        for (const auto& item : list->Items) {
            if (item->Star) {
                std::string star = item->StarPrefix.empty()
                    ? "*"
                    : Join(item->StarPrefix, '.') + ".*";
                Line(ind + 2, "(star " + star + ")");
            } else {
                std::string head = "(item";
                if (item->Alias) {
                    head += " as " + *item->Alias;
                }
                Line(ind + 2, head);
                Expr(ind + 4, item->Expr);
                Line(ind + 2, ")");
            }
        }
        Line(ind, ")");
    }

    void From(int ind, const TSqlPtr<TSqlFrom>& from) {
        Line(ind, "(from");
        for (const auto& ref : from->Items) {
            TableRef(ind + 2, ref);
        }
        Line(ind, ")");
    }

    void TableRef(int ind, const TSqlPtr<TSqlTableRef>& ref) {
        if (auto name = std::dynamic_pointer_cast<TSqlTableName>(ref)) {
            std::string head = "(table " + Join(name->Name, '.');
            if (name->Alias) {
                head += " as " + *name->Alias;
            }
            head += ")";
            Line(ind, head);
            return;
        }
        if (auto sub = std::dynamic_pointer_cast<TSqlSubqueryTable>(ref)) {
            std::string head = "(subquery-table";
            if (sub->Alias) {
                head += " as " + *sub->Alias;
            }
            if (sub->ColumnAliases) {
                head += " cols: " + Join(sub->ColumnAliases->Items, ' ');
            }
            Line(ind, head);
            Query(ind + 2, sub->Query);
            Line(ind, ")");
            return;
        }
        if (auto join = std::dynamic_pointer_cast<TSqlJoin>(ref)) {
            JoinNode(ind, join);
            return;
        }
        Line(ind, "(unknown-table)");
    }

    void JoinNode(int ind, const TSqlPtr<TSqlJoin>& j) {
        Line(ind, "(join " + JoinTypeName(j->Type));
        TableRef(ind + 2, j->Left);
        TableRef(ind + 2, j->Right);
        if (j->Condition) {
            if (j->Condition->On) {
                Line(ind + 2, "(on");
                Expr(ind + 4, j->Condition->On);
                Line(ind + 2, ")");
            } else if (j->Condition->UsingColumns) {
                IdentList(ind + 2, "(using", j->Condition->UsingColumns);
            }
        }
        Line(ind, ")");
    }

    void GroupBy(int ind, const TSqlPtr<TSqlGroupBy>& gb) {
        Line(ind, "(group-by");
        for (const auto& item : gb->Items) {
            Expr(ind + 2, item);
        }
        Line(ind, ")");
    }

    void Order(int ind, const TSqlPtr<TSqlOrder>& order) {
        Line(ind, "(order-by");
        for (const auto& item : order->Items) {
            std::string head = item->Desc ? "(order desc" : "(order asc";
            if (item->NullOrder == TSqlOrderItem::ENullOrder::First) {
                head += " nulls-first";
            } else if (item->NullOrder == TSqlOrderItem::ENullOrder::Last) {
                head += " nulls-last";
            }
            Line(ind + 2, head);
            Expr(ind + 4, item->Expr);
            Line(ind + 2, ")");
        }
        Line(ind, ")");
    }
};

} // namespace

std::string PrintAst(const TSqlNodePtr& node) {
    std::ostringstream out;
    TSqlPrinter printer{out};

    if (auto query = std::dynamic_pointer_cast<TSqlQuery>(node)) {
        printer.Query(0, query);
    } else if (node) {
        printer.Body(0, node);
    } else {
        out << "(null)\n";
    }

    return out.str();
}

} // namespace NSql
} // namespace NQdb
