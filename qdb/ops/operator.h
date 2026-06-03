#pragma once

#include <qdb/io/io.h>

#include <qumir/parser/ast.h>

namespace NQqb {

// Base for all relational operators.
//
// Inherits TExpr so the operator tree is a first-class AST:
//   - Type      : output TStructType (fields = columns)
//   - Children(): input operators
//   - ToString(): core-lang style "(rel <name> ...)"
//
// Future: parsed directly from "(rel filter ...)" in core-lang.
struct IOperator : NQumir::NAst::TExpr {
    static constexpr const char* NodeId = "Rel";

    const std::string_view NodeName() const override { return NodeId; }

    // Specific operator name: "source", "filter", "project"
    virtual std::string_view RelName() const = 0;

    void Accept(NQumir::NAst::IVisitor& visitor) override { visitor.VisitOtherwise(*this); }
};

using TOperatorPtr = std::shared_ptr<IOperator>;

// Analogous to TMaybeNode<T> in qumir.
// Checks NodeName() == "Rel" AND RelName() == T::OpId.
template<typename T>
struct TMaybeOp {
    explicit TMaybeOp(TOperatorPtr node)
        : Op(node
            && std::string_view(IOperator::NodeId) == node->NodeName()
            && std::string_view(T::OpId) == node->RelName()
            ? std::static_pointer_cast<T>(std::move(node))
            : nullptr)
    {}

    std::shared_ptr<T> Cast() const {
        return std::static_pointer_cast<T>(Op);
    }

    TOperatorPtr Op;
    operator bool() const { return Op != nullptr; }
};

} // namespace NQqb
