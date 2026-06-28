#include <qdb/kernel/spec.h>

#include <ostream>

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

