#include <qdb/ops/project.h>
#include <qdb/ops/parse_kernel.h>

#include <qumir/parser/ast.h>

namespace NQqb {

TProjectOperator::TProjectOperator(
    TOperatorPtr input,
    std::vector<TProjectionSpec> projections,
    std::vector<NQumir::NAst::TExprPtr> kernels)
    : Input_(std::move(input))
    , Projections_(std::move(projections))
    , Kernels_(std::move(kernels))
{}

const std::string TProjectOperator::ToString() const {
    std::string s = "(rel project " + Input_->ToString();
    for (const auto& k : Kernels_) {
        auto* fun = static_cast<NQumir::NAst::TFunDecl*>(k.get());
        s += " (";
        s += fun ? fun->Name : "?";
        s += ")";
    }
    s += ")";
    return s;
}

std::expected<TOperatorPtr, NQumir::TError>
MakeProject(TOperatorPtr input, std::vector<TProjectionSpec> projections) {
    using namespace NQumir::NAst;

    auto* structType = static_cast<TStructType*>(input->Type.get());
    if (!structType) {
        return std::unexpected(NQumir::TError("project input must have TStructType"));
    }

    std::vector<TExprPtr> kernels;
    std::vector<std::pair<std::string, TTypePtr>> outFields;
    kernels.reserve(projections.size());
    outFields.reserve(projections.size());

    for (auto& spec : projections) {
        auto kernel = NInternal::ParseAndAnnotate(*structType, spec.Expression);
        if (!kernel) {
            return std::unexpected(kernel.error());
        }

        // Extract output type from the annotated function body's first statement
        auto* fun = static_cast<TFunDecl*>(kernel->get());
        TTypePtr exprType;
        if (fun && fun->Body && !fun->Body->Stmts.empty()) {
            exprType = fun->Body->Stmts[0]->Type;
        }

        outFields.emplace_back(spec.Name, exprType);
        kernels.push_back(std::move(*kernel));
    }

    auto op = std::make_shared<TProjectOperator>(std::move(input), std::move(projections), std::move(kernels));
    op->Type = std::make_shared<TStructType>(std::move(outFields));
    return op;
}

} // namespace NQqb
