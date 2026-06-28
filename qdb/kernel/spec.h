#pragma once

#include <qumir/parser/type.h>

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace NQdb {
namespace NKernel {

enum class EOperatorKernelKind {
    NonCompute,
    UnaryStreaming,
    UnaryBlocking,
    Binary,
};

struct TKernelColumnRef {
    std::string Name;
    int32_t Index = -1;
    NQumir::NAst::TTypePtr Type;
};

struct TKernelKeySpec {
    std::string Name;
    std::vector<TKernelColumnRef> Columns;
};

struct TKernelEntrypointSpec {
    std::string Name;
    std::string Abi;
};

struct TOperatorKernelSpec {
    EOperatorKernelKind Kind = EOperatorKernelKind::NonCompute;
    std::string OperatorName;
    std::vector<NQumir::NAst::TTypePtr> InputSchemas;
    NQumir::NAst::TTypePtr OutputSchema;
    std::vector<TKernelColumnRef> ReferencedColumns;
    std::vector<TKernelKeySpec> Keys;
    std::vector<TKernelEntrypointSpec> Entrypoints;
    std::vector<std::string> SourceModules;
};

std::string_view KernelKindName(EOperatorKernelKind kind);

void PrintKernelSpec(std::ostream& out, const TOperatorKernelSpec& spec);

} // namespace NKernel
} // namespace NQdb

