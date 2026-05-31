#include <qdb/kernel/compiler.h>
#include <qdb/kernel/gen.h>

#include <qumir/codegen/llvm/llvm_initializer.h>

#include <stdexcept>

namespace NQqb {

TKernelCompiler::TFilterDispatch TKernelCompiler::CompileFilter(
    const NQumir::NAst::TStructType& inputType,
    const NQumir::NAst::TExprPtr& predicate)
{
    auto kernelAst = NKernel::GenFilterKernelAst(predicate, inputType);

    NQumir::TLLVMRunnerOptions opts;
    opts.CoreInput = true;
    opts.ResolveCoreInput = true;
    opts.OptLevel = 3;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(opts);

    std::string err;
    void* fnPtr = runner->CompileKernelAst(std::move(kernelAst), &err);
    if (!fnPtr) {
        throw std::runtime_error("filter kernel compilation failed: " + err);
    }

    using TFilterFn = void(*)(void*);

    struct TLayout {
        int32_t ColumnCount;
        std::vector<int32_t> ColIndices;
    };

    TLayout layout;
    layout.ColumnCount = static_cast<int32_t>(inputType.Fields.size());
    for (int32_t i = 0; i < layout.ColumnCount; ++i) {
        layout.ColIndices.push_back(i);
    }

    auto sharedRunner = std::shared_ptr<NQumir::TLLVMRunner>(std::move(runner));

    TFilterDispatch dispatch = [fnPtr, layout, sharedRunner](
        const TRowSet& rowSet, bool* selection)
    {
        std::vector<uintptr_t> ctx;
        ctx.reserve(static_cast<size_t>(layout.ColumnCount) + 2);
        ctx.push_back(static_cast<uintptr_t>(rowSet.RowCount));
        ctx.push_back(reinterpret_cast<uintptr_t>(selection));
        for (int32_t i = 0; i < layout.ColumnCount; ++i) {
            ctx.push_back(reinterpret_cast<uintptr_t>(
                rowSet.Columns[layout.ColIndices[i]].Data));
        }
        reinterpret_cast<TFilterFn>(fnPtr)(ctx.data());
    };

    return dispatch;
}

} // namespace NQqb
