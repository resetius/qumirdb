#include <qdb/kernel/compiler.h>
#include <qdb/kernel/gen.h>
#include <qdb/modules/qumirdb.h>

#include <qumir/codegen/llvm/llvm_initializer.h>
#include <qumir/parser/core/printer.h>

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
    opts.NativeCode = true;
    opts.PrintAsm = true;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(opts);
    runner->RegisterModule(std::make_shared<NQumir::NRegistry::QumirDbModule>(), true);

    NQumir::NAst::NCore::PrintAst(std::cerr, kernelAst);

    std::string err;
    void* fnPtr = runner->CompileKernelAst(std::move(kernelAst), &err);
    if (!fnPtr) {
        throw std::runtime_error("filter kernel compilation failed: " + err);
    }

    using TFilterFn = void(*)(void*);

    struct TLayout {
        int32_t ColumnCount;
        std::vector<int32_t> ColIndices;
        std::vector<bool> IsBoolColumn;
    };

    TLayout layout;
    layout.ColumnCount = static_cast<int32_t>(inputType.Fields.size());
    int32_t boolColumns = 0;
    for (int32_t i = 0; i < layout.ColumnCount; ++i) {
        layout.ColIndices.push_back(i);
        bool isBool = NQumir::NAst::TMaybeType<NQumir::NAst::TBoolType>(inputType.Fields[i].second);
        layout.IsBoolColumn.push_back(isBool);
        boolColumns += isBool ? 1 : 0;
    }

    auto sharedRunner = std::shared_ptr<NQumir::TLLVMRunner>(std::move(runner));

    TFilterDispatch dispatch = [fnPtr, layout, sharedRunner, boolColumns](
        const TRowSet& rowSet, uint8_t* selection)
    {
        std::vector<uintptr_t> ctx;
        ctx.reserve(static_cast<size_t>(layout.ColumnCount) + static_cast<size_t>(boolColumns) + 2);
        ctx.push_back(static_cast<uintptr_t>(rowSet.RowCount));
        ctx.push_back(reinterpret_cast<uintptr_t>(selection));
        for (int32_t i = 0; i < layout.ColumnCount; ++i) {
            const auto& col = rowSet.Columns[layout.ColIndices[i]];
            ctx.push_back(reinterpret_cast<uintptr_t>(col.Data));
            if (layout.IsBoolColumn[i]) {
                ctx.push_back(static_cast<uintptr_t>(col.DataBitOffset));
            }
        }
        reinterpret_cast<TFilterFn>(fnPtr)(ctx.data());
    };

    return dispatch;
}

} // namespace NQqb
