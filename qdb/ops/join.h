#pragma once

#include <qdb/ops/operator.h>

#include <qumir/error.h>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace NQqb {

enum class EJoinType {
    Inner,
    Left,       // LEFT OUTER
    Right,      // RIGHT OUTER
    Full,       // FULL OUTER
    LeftSemi,
    RightSemi,
    LeftAnti,
    RightAnti,
};

// Equi-join key: a pair of column names, one from each side
// (left.Left == right.Right is the equality predicate).
struct TJoinKey {
    std::string Left;
    std::string Right;
};

// String <-> enum helpers (used by sexp and ToString).
std::string_view JoinTypeName(EJoinType type);
std::optional<EJoinType> ParseJoinType(std::string_view name);

class TJoinOperator : public IOperator {
public:
    static constexpr const char* OpId = "join";

    // filter: optional residual predicate θ over the joined (left⊕right) row,
    // applied at the match point BEFORE a pair is emitted. nullptr if none.
    TJoinOperator(TOperatorPtr left, TOperatorPtr right,
        std::vector<TJoinKey> keys, EJoinType type,
        NQumir::NAst::TExprPtr filter);

    std::string_view RelName() const override { return OpId; }
    std::unordered_set<std::string> ComputeReferencedColumns() const override;
    // First operator with TWO children.
    std::vector<NQumir::NAst::TExprPtr> Children() const override { return {Left_, Right_}; }
    const std::string ToString() const override;

    TOperatorPtr Left() const { return Left_; }
    TOperatorPtr Right() const { return Right_; }
    const std::vector<TJoinKey>& Keys() const { return Keys_; }
    EJoinType JoinType() const { return Type_; }
    // Residual predicate, applied before emit; nullptr if absent.
    const NQumir::NAst::TExprPtr& Filter() const { return Filter_; }

private:
    TOperatorPtr Left_, Right_;
    std::vector<TJoinKey> Keys_;
    EJoinType Type_;
    NQumir::NAst::TExprPtr Filter_; // parsed, unannotated; may be null
};

// Output schema by join type:
//   Inner: left ++ right
//   Left:  left ++ nullable(right)
//   Right: nullable(left) ++ right
//   Full:  nullable(left) ++ nullable(right)
//   Left{Semi,Anti}:  left columns only
//   Right{Semi,Anti}: right columns only
// Returns an error if both sides contribute columns and column names overlap
// (no automatic prefixing/aliasing yet).
std::expected<NQumir::NAst::TTypePtr, NQumir::TError> ComputeJoinOutputType(
    const NQumir::NAst::TTypePtr& left,
    const NQumir::NAst::TTypePtr& right,
    EJoinType type);

// keys: list of (leftCol, rightCol) pairs; must be non-empty.
// filter: residual predicate expression string; empty for no residual.
std::expected<TOperatorPtr, NQumir::TError>
MakeJoin(TOperatorPtr left, TOperatorPtr right,
    std::vector<std::pair<std::string, std::string>> keys,
    EJoinType type,
    const std::string& filter = "");

} // namespace NQqb
