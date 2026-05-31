#include <qdb/kernel/gen.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/operator.h>
#include <qumir/location.h>

namespace NQqb {
namespace NKernel {

void SubstFieldsInPlace(
    NQumir::NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& fieldNames,
    const std::unordered_set<std::string>& boolFieldNames,
    const NQumir::NAst::TExprPtr& indexIdent)
{
    if (!expr) {
        return;
    }
    if (auto node = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(expr)) {
        if (fieldNames.count(node.Cast()->Name)) {
            if (boolFieldNames.count(node.Cast()->Name)) {
                expr = std::make_shared<NQumir::NAst::TCallExpr>(
                    expr->Location,
                    std::make_shared<NQumir::NAst::TIdentExpr>(expr->Location, "bitoff"),
                    std::vector<NQumir::NAst::TExprPtr>{
                        expr,
                        indexIdent,
                        std::make_shared<NQumir::NAst::TIdentExpr>(expr->Location, node.Cast()->Name + "$bitoff")
                    });
            } else {
                expr = std::make_shared<NQumir::NAst::TIndexExpr>(
                    expr->Location, expr, indexIdent);
            }
            return;
        }
        return;
    }
    for (auto* child : expr->MutableChildren()) {
        SubstFieldsInPlace(*child, fieldNames, boolFieldNames, indexIdent);
    }
}

// Kernel ABI: single ctx parameter of type <ref <struct (n i64) (selection <ptr u8>) (col <ptr T>)...>>.
// LLVM compiles this to a single pointer argument, so calling as fn(ctx_array.data()) is correct.
NQumir::NAst::TExprPtr GenFilterKernelAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& inputType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    std::unordered_set<std::string> fieldNames;
    std::unordered_set<std::string> boolFieldNames;
    for (const auto& [name, _] : inputType.Fields) {
        fieldNames.insert(name);
    }

    auto identI = std::make_shared<TIdentExpr>(loc, "i");
    for (const auto& [name, type] : inputType.Fields) {
        if (TMaybeType<TBoolType>(type)) {
            boolFieldNames.insert(name);
        }
    }
    SubstFieldsInPlace(predicate, fieldNames, boolFieldNames, identI);

    // Build the context struct type: (n i64) (selection <ptr u8>) (col <ptr T>)...
    std::vector<std::pair<std::string, TTypePtr>> ctxFields;
    ctxFields.emplace_back("n", std::make_shared<TIntegerType>());
    ctxFields.emplace_back("selection",
        std::make_shared<TPointerType>(std::make_shared<TIntegerType>(TIntegerType::U8)));
    for (const auto& [name, type] : inputType.Fields) {
        if (TMaybeType<TBoolType>(type)) {
            ctxFields.emplace_back(name, std::make_shared<TPointerType>(std::make_shared<TIntegerType>(TIntegerType::U8)));
        } else {
            ctxFields.emplace_back(name, std::make_shared<TPointerType>(type));
        }
    }
    auto ctxStructType = std::make_shared<TStructType>(ctxFields);
    auto ctxRefType    = std::make_shared<TReferenceType>(ctxStructType);

    // Single param: (var ctx <ref <struct ...>>)
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "ctx", ctxRefType),
    };

    // Function body: extract all fields from ctx, then loop
    std::vector<TExprPtr> bodyStmts;
    auto identCtx = std::make_shared<TIdentExpr>(loc, "ctx");

    auto extractField = [&](const std::string& name, TTypePtr type) {
        bodyStmts.push_back(
            std::make_shared<TVarStmt>(loc, name, type));
        bodyStmts.push_back(
            std::make_shared<TAssignExpr>(loc, name,
                std::make_shared<TFieldAccessExpr>(loc, identCtx, name)));
    };

    extractField("n", std::make_shared<TIntegerType>());
    extractField("selection",
        std::make_shared<TPointerType>(std::make_shared<TIntegerType>(TIntegerType::U8)));
    for (const auto& [name, type] : inputType.Fields) {
        if (TMaybeType<TBoolType>(type)) {
            extractField(name, std::make_shared<TPointerType>(std::make_shared<TIntegerType>(TIntegerType::U8)));
            extractField(name + "$bitoff", std::make_shared<TIntegerType>());
        } else {
            extractField(name, std::make_shared<TPointerType>(type));
        }
    }

    // Loop
    auto varI  = std::make_shared<TVarStmt>(loc, "i", std::make_shared<TIntegerType>());
    auto initI = std::make_shared<TAssignExpr>(loc, "i",
                     std::make_shared<TNumberExpr>(loc, int64_t(0)));
    auto cond  = std::make_shared<TBinaryExpr>(loc, TOperator("<"),
                     std::make_shared<TIdentExpr>(loc, "i"),
                     std::make_shared<TIdentExpr>(loc, "n"));
    auto writeSel = std::make_shared<TArrayAssignExpr>(loc, "selection",
                        std::vector<TExprPtr>{std::make_shared<TIdentExpr>(loc, "i")},
                        std::make_shared<TCastExpr>(loc, predicate,
                            std::make_shared<TIntegerType>(TIntegerType::U8)));
    auto incrI = std::make_shared<TAssignExpr>(loc, "i",
                     std::make_shared<TBinaryExpr>(loc, TOperator("+"),
                         std::make_shared<TIdentExpr>(loc, "i"),
                         std::make_shared<TNumberExpr>(loc, int64_t(1))));

    bodyStmts.push_back(varI);
    bodyStmts.push_back(initI);
    bodyStmts.push_back(std::make_shared<TWhileStmtExpr>(loc, cond,
        std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{writeSel, incrI})));

    auto funBody = std::make_shared<TBlockExpr>(loc, std::move(bodyStmts));
    auto funDecl = std::make_shared<TFunDecl>(loc, "<kernel>",
                       std::move(params), funBody,
                       std::make_shared<TVoidType>());

    return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{
        std::make_shared<TUseExpr>(loc, "QumirDb"),
        funDecl
    });
}

} // namespace NKernel
} // namespace NQqb
