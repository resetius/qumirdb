#include <qdb/kernel/spec.h>

#include <qdb/plan/passes/unbound_vars.h>

#include <qumir/parser/core/printer.h>

#include <algorithm>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_set>

namespace NQdb {
namespace NKernel {

namespace {

std::string TypeName(const NQumir::NAst::TTypePtr& type) {
    return type ? type->ToString() : std::string("<null>");
}

void PrintColumn(std::ostream& out, const TKernelColumnRef& column) {
    out << column.Name << "#" << column.Index << ":" << TypeName(column.Type);
}

} // namespace

std::string_view KernelKindName(EOperatorKernelKind kind) {
    switch (kind) {
        case EOperatorKernelKind::NonCompute:
            return "non-compute";
        case EOperatorKernelKind::UnaryStreaming:
            return "unary-streaming";
        case EOperatorKernelKind::UnaryBlocking:
            return "unary-blocking";
        case EOperatorKernelKind::Binary:
            return "binary";
    }
    return "unknown";
}

TOperatorKernelSpec BuildFilterKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const NQumir::NAst::TExprPtr& predicate,
    std::string entrypointName)
{
    const auto refs = FindUnboundVars(predicate);

    std::vector<TKernelColumnRef> referenced;
    for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
        const auto& [name, type] = inputType.Fields[i];
        if (refs.contains(name)) {
            referenced.push_back({.Name = name, .Index = i, .Type = type});
        }
    }

    std::ranges::sort(referenced, [](const auto& lhs, const auto& rhs) {
        return lhs.Index < rhs.Index;
    });

    return TOperatorKernelSpec{
        .Kind = EOperatorKernelKind::UnaryStreaming,
        .OperatorName = "filter",
        .InputSchemas = {std::make_shared<NQumir::NAst::TStructType>(inputType.Fields)},
        .OutputSchema = std::make_shared<NQumir::NAst::TStructType>(inputType.Fields),
        .ReferencedColumns = std::move(referenced),
        .Expressions = {predicate},
        .Entrypoints = {
            {.Name = std::move(entrypointName), .Abi = "void(ref TRowSet)"},
        },
        .SourceModules = {"qumirdb"},
    };
}

TOperatorKernelSpec BuildProjectKernelSpec(
    const NQumir::NAst::TStructType& inputType,
    const std::vector<NQumir::NAst::TExprPtr>& computedExprs,
    const std::vector<NQumir::NAst::TTypePtr>& computedTypes,
    std::string entrypointName)
{
    std::vector<std::pair<std::string, NQumir::NAst::TTypePtr>> outputFields;
    outputFields.reserve(computedTypes.size());
    for (size_t i = 0; i < computedTypes.size(); ++i) {
        outputFields.emplace_back("computed_" + std::to_string(i), computedTypes[i]);
    }

    std::unordered_set<std::string> refs;
    for (const auto& expr : computedExprs) {
        auto exprRefs = FindUnboundVars(expr);
        refs.insert(exprRefs.begin(), exprRefs.end());
    }

    std::vector<TKernelColumnRef> referenced;
    for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
        const auto& [name, type] = inputType.Fields[i];
        if (refs.contains(name)) {
            referenced.push_back({.Name = name, .Index = i, .Type = type});
        }
    }

    std::ranges::sort(referenced, [](const auto& lhs, const auto& rhs) {
        return lhs.Index < rhs.Index;
    });

    return TOperatorKernelSpec{
        .Kind = EOperatorKernelKind::UnaryStreaming,
        .OperatorName = "project-compute",
        .InputSchemas = {std::make_shared<NQumir::NAst::TStructType>(inputType.Fields)},
        .OutputSchema = std::make_shared<NQumir::NAst::TStructType>(std::move(outputFields)),
        .ReferencedColumns = std::move(referenced),
        .Expressions = computedExprs,
        .Entrypoints = {
            {.Name = std::move(entrypointName), .Abi = "void(ref TRowSet, ptr ptr i8)"},
        },
        .SourceModules = {"qumirdb"},
    };
}

void PrintKernelSpec(std::ostream& out, const TOperatorKernelSpec& spec) {
    out << "kernel-spec " << spec.OperatorName << "\n";
    out << "  kind: " << KernelKindName(spec.Kind) << "\n";

    out << "  inputs:";
    if (spec.InputSchemas.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (size_t i = 0; i < spec.InputSchemas.size(); ++i) {
            out << "    [" << i << "] " << TypeName(spec.InputSchemas[i]) << "\n";
        }
    }

    out << "  output: " << TypeName(spec.OutputSchema) << "\n";

    out << "  referenced:";
    if (spec.ReferencedColumns.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (const auto& column : spec.ReferencedColumns) {
            out << "    ";
            PrintColumn(out, column);
            out << "\n";
        }
    }

    out << "  expressions:";
    if (spec.Expressions.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (size_t i = 0; i < spec.Expressions.size(); ++i) {
            out << "    [" << i << "] ";
            if (spec.Expressions[i]) {
                out << NQumir::NAst::NCore::PrintAst(
                    spec.Expressions[i],
                    NQumir::NAst::NCore::TPrintOptions{.Pretty = false});
            } else {
                out << "<null>";
            }
            out << "\n";
        }
    }

    out << "  keys:";
    if (spec.Keys.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (const auto& key : spec.Keys) {
            out << "    " << key.Name << ":";
            if (key.Columns.empty()) {
                out << " []";
            } else {
                for (const auto& column : key.Columns) {
                    out << " ";
                    PrintColumn(out, column);
                }
            }
            out << "\n";
        }
    }

    out << "  entrypoints:";
    if (spec.Entrypoints.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (const auto& entry : spec.Entrypoints) {
            out << "    " << entry.Name << ": " << entry.Abi << "\n";
        }
    }

    out << "  source-modules:";
    if (spec.SourceModules.empty()) {
        out << " []\n";
    } else {
        out << "\n";
        for (const auto& module : spec.SourceModules) {
            out << "    " << module << "\n";
        }
    }
}

} // namespace NKernel
} // namespace NQdb
