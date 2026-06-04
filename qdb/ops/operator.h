#pragma once

#include <qdb/io/io.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/type.h>

#include <string>
#include <unordered_set>

namespace NQqb {

struct IOperator : NQumir::NAst::TExpr {
    static constexpr const char* NodeId = "Rel";

    const std::string_view NodeName() const override { return NodeId; }
    virtual std::string_view RelName() const = 0;
    void Accept(NQumir::NAst::IVisitor& visitor) override { visitor.VisitOtherwise(*this); }

    // Columns referenced in this node's own expressions (predicate / projections).
    // Excludes what input operators produce — only own references.
    virtual std::unordered_set<std::string> ComputeReferencedColumns() const = 0;

    // Output schema: TFunctionType::ReturnType.
    NQumir::NAst::TTypePtr OutputColumns() const {
        return static_cast<NQumir::NAst::TFunctionType*>(Type.get())->ReturnType;
    }

    // Required input schema: TFunctionType::ParamTypes[0].
    // Null for source (no upstream operator) and before typing pass.
    NQumir::NAst::TTypePtr RequiredColumns() const {
        auto* fun = static_cast<NQumir::NAst::TFunctionType*>(Type.get());
        return fun->ParamTypes.empty() ? nullptr : fun->ParamTypes[0];
    }
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
