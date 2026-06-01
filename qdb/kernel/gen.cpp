#include <qdb/kernel/gen.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/operator.h>
#include <qumir/location.h>

namespace NQqb {
namespace NKernel {

void SubstFieldsInPlace(
    NQumir::NAst::TExprPtr& expr,
    const std::unordered_set<std::string>& fieldNames,
    const NQumir::NAst::TExprPtr& indexIdent)
{
    if (!expr) {
        return;
    }
    if (auto node = NQumir::NAst::TMaybeNode<NQumir::NAst::TIdentExpr>(expr)) {
        if (fieldNames.count(node.Cast()->Name)) {
            expr = std::make_shared<NQumir::NAst::TIndexExpr>(
                expr->Location, expr, indexIdent);
            return;
        }
        return;
    }
    for (auto* child : expr->MutableChildren()) {
        SubstFieldsInPlace(*child, fieldNames, indexIdent);
    }
}

// Kernel takes (ref TRowSet) directly. Column data pointers are extracted via
// TRowSet.Columns[colIdx].Data with a two-step cast: <ptr i8> -> i64 -> <ptr T>.
NQumir::NAst::TExprPtr GenFilterKernelAst(
    NQumir::NAst::TExprPtr predicate,
    const NQumir::NAst::TStructType& inputType,
    const std::unordered_map<std::string, int32_t>& fieldIndices,
    NQumir::NAst::TTypePtr columnType,
    NQumir::NAst::TTypePtr rowSetType)
{
    using namespace NQumir::NAst;
    NQumir::TLocation loc{};

    std::unordered_set<std::string> fieldNames;
    for (const auto& [name, _] : inputType.Fields) {
        fieldNames.insert(name);
    }

    auto identI = std::make_shared<TIdentExpr>(loc, "i");
    SubstFieldsInPlace(predicate, fieldNames, identI);

    // Single param: (var rowSet <ref TRowSet>) — raw struct type, no TNamedType wrapper
    auto rowSetRefType = std::make_shared<TReferenceType>(rowSetType);
    std::vector<TParam> params = {
        std::make_shared<TVarStmt>(loc, "rowSet", rowSetRefType),
    };

    std::vector<TExprPtr> bodyStmts;
    auto identRowSet = std::make_shared<TIdentExpr>(loc, "rowSet");

    auto fieldOf = [&](const std::string& name) {
        return std::make_shared<TFieldAccessExpr>(loc, identRowSet, name);
    };

    // Extract n, selection, cols from rowSet
    bodyStmts.push_back(std::make_shared<TVarStmt>(loc, "n", std::make_shared<TIntegerType>()));
    bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, "n", fieldOf("RowCount")));

    auto ptrU8Type = std::make_shared<TPointerType>(
        std::make_shared<TIntegerType>(TIntegerType::U8));
    bodyStmts.push_back(std::make_shared<TVarStmt>(loc, "selection", ptrU8Type));
    bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, "selection", fieldOf("Selection")));

    // Use the exact Columns field type from rowSetType to avoid type mismatches.
    auto ptrColumnType = [&]() -> TTypePtr {
        auto* rs = static_cast<TStructType*>(rowSetType.get());
        for (const auto& [name, type] : rs->Fields) {
            if (name == "Columns") {
                return type;
            }
        }
        return std::make_shared<TPointerType>(columnType);
    }();
    bodyStmts.push_back(std::make_shared<TVarStmt>(loc, "cols", ptrColumnType));
    bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, "cols", fieldOf("Columns")));

    // For each referenced field: cols[colIdx].Data -> i64 -> <ptr T>
    auto i64Type = std::make_shared<TIntegerType>();
    for (const auto& [name, type] : inputType.Fields) {
        int32_t idx = fieldIndices.at(name);
        auto ptrFieldType = std::make_shared<TPointerType>(type);
        auto colElem = std::make_shared<TIndexExpr>(loc,
            std::make_shared<TIdentExpr>(loc, "cols"),
            std::make_shared<TNumberExpr>(loc, int64_t(idx)));
        auto rawData = std::make_shared<TFieldAccessExpr>(loc, colElem, "Data");
        auto asInt = std::make_shared<TCastExpr>(loc, rawData, i64Type);
        auto asTypedPtr = std::make_shared<TCastExpr>(loc, asInt, ptrFieldType);

        bodyStmts.push_back(std::make_shared<TVarStmt>(loc, name, ptrFieldType));
        bodyStmts.push_back(std::make_shared<TAssignExpr>(loc, name, asTypedPtr));
    }

    // Loop
    auto varI = std::make_shared<TVarStmt>(loc, "i", std::make_shared<TIntegerType>());
    auto initI = std::make_shared<TAssignExpr>(loc, "i",
        std::make_shared<TNumberExpr>(loc, int64_t(0)));
    auto cond = std::make_shared<TBinaryExpr>(loc, TOperator("<"),
        std::make_shared<TIdentExpr>(loc, "i"),
        std::make_shared<TIdentExpr>(loc, "n"));
    auto castedPred = std::make_shared<TCastExpr>(loc, predicate,
        std::make_shared<TIntegerType>(TIntegerType::U8));
    auto writeSel = std::make_shared<TArrayAssignExpr>(loc, "selection",
        std::vector<TExprPtr>{std::make_shared<TIdentExpr>(loc, "i")},
        castedPred);
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
        std::move(params), funBody, std::make_shared<TVoidType>());

    return std::make_shared<TBlockExpr>(loc, std::vector<TExprPtr>{funDecl});
}

} // namespace NKernel
} // namespace NQqb
