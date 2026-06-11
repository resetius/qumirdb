#include <qdb/ops/aggregate.h>

#include <qdb/pipeline/unbound_vars.h>

#include <qumir/parser/ast.h>
#include <qumir/parser/core/lexer.h>
#include <qumir/parser/core/parser.h>
#include <qumir/parser/core/printer.h>
#include <qumir/parser/type.h>

#include <sstream>

namespace NQqb {

using namespace NQumir::NAst;

namespace {

TTypePtr FieldType(const TStructType* inputStruct, const std::string& name) {
    if (!inputStruct) {
        return nullptr;
    }
    for (const auto& [fieldName, type] : inputStruct->Fields) {
        if (fieldName == name) {
            return type;
        }
    }
    return nullptr;
}

TTypePtr AggResultType(const std::string& func, const TTypePtr& argType) {
    if (func == "count") {
        return std::make_shared<TIntegerType>(); // i64
    }
    if (func == "sum" && argType && argType->TypeName() == TIntegerType::TypeId) {
        return std::make_shared<TIntegerType>(); // sum of integers promotes to i64
    }
    // sum (non-integer) / min / max: result has the same type as the argument.
    return argType;
}

} // namespace

TTypePtr ComputeAggregateOutputType(
    const TTypePtr& inputSchema,
    const std::vector<std::string>& groupKeys,
    const std::vector<TAggregateSpec>& aggs)
{
    auto* inputStruct = static_cast<TStructType*>(inputSchema.get());
    std::vector<std::pair<std::string, TTypePtr>> outFields;
    for (const auto& key : groupKeys) {
        outFields.emplace_back(key, FieldType(inputStruct, key));
    }
    for (const auto& agg : aggs) {
        TTypePtr argType;
        if (agg.Arg) {
            if (auto ident = TMaybeNode<TIdentExpr>(agg.Arg)) {
                argType = FieldType(inputStruct, ident.Cast()->Name);
            }
        }
        outFields.emplace_back(agg.Name, AggResultType(agg.Func, argType));
    }
    return std::make_shared<TStructType>(std::move(outFields));
}

TAggregateOperator::TAggregateOperator(TOperatorPtr input, std::vector<std::string> groupKeys, std::vector<TAggregateSpec> aggs)
    : Input_(std::move(input))
    , GroupKeys_(std::move(groupKeys))
    , Aggs_(std::move(aggs))
{
    auto inputSchema = Input_->OutputColumns();
    Type = std::make_shared<TFunctionType>(
        std::vector<TTypePtr>{inputSchema},
        ComputeAggregateOutputType(inputSchema, GroupKeys_, Aggs_));
}

std::unordered_set<std::string> TAggregateOperator::ComputeReferencedColumns() const {
    std::unordered_set<std::string> refs;
    for (const auto& key : GroupKeys_) {
        refs.insert(key);
    }
    for (const auto& agg : Aggs_) {
        if (agg.Arg) {
            for (auto& col : FindUnboundVars(agg.Arg)) {
                refs.insert(col);
            }
        }
    }
    return refs;
}

const std::string TAggregateOperator::ToString() const {
    std::string s = "(rel aggregate " + Input_->ToString();
    s += " (keys";
    for (const auto& key : GroupKeys_) {
        s += " " + key;
    }
    s += ")";
    for (const auto& agg : Aggs_) {
        s += " (agg " + agg.Name + " " + agg.Func;
        if (agg.Arg) {
            s += " " + NQumir::NAst::NCore::PrintAst(agg.Arg);
        }
        s += ")";
    }
    return s + ")";
}

std::expected<TOperatorPtr, NQumir::TError>
MakeAggregate(TOperatorPtr input, std::vector<std::string> groupKeys,
    std::vector<std::tuple<std::string, std::string, std::string>> aggs)
{
    std::vector<TAggregateSpec> specs;
    specs.reserve(aggs.size());
    for (auto& [name, func, exprStr] : aggs) {
        TExprPtr arg;
        if (!exprStr.empty()) {
            std::istringstream ss(exprStr);
            NQumir::NAst::NCore::TTokenStream tokens(ss);
            NQumir::NAst::NCore::TParser parser;
            auto parsed = parser.Parse(tokens);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            arg = std::move(*parsed);
        }
        specs.push_back({std::move(name), std::move(func), std::move(arg)});
    }
    return std::make_shared<TAggregateOperator>(std::move(input), std::move(groupKeys), std::move(specs));
}

} // namespace NQqb
