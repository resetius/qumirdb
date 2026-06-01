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
    std::unordered_map<std::string, int32_t> fieldIndices;
    for (int32_t i = 0; i < static_cast<int32_t>(inputType.Fields.size()); ++i) {
        fieldIndices[inputType.Fields[i].first] = i;
    }

    auto dbModule = std::make_shared<NQumir::NRegistry::QumirDbModule>();
    const auto& extTypes = dbModule->ExternalTypes();
    // ExternalTypes_[0] = TColumn, [1] = TRowSet (order from qumirdb.cpp)
    NQumir::NAst::TTypePtr columnType;
    NQumir::NAst::TTypePtr rowSetType;
    for (const auto& et : extTypes) {
        if (et.Name == "TColumn") {
            columnType = et.Type;
        } else if (et.Name == "TRowSet") {
            rowSetType = et.Type;
        }
    }

    auto kernelAst = NKernel::GenFilterKernelAst(
        predicate, inputType, fieldIndices, columnType, rowSetType);

    NQumir::TLLVMRunnerOptions opts;
    opts.CoreInput = true;
    opts.ResolveCoreInput = true;
    opts.OptLevel = 3;
    opts.NativeCode = true;
    opts.PrintAsm = true;
    opts.PrintIr = true;
    auto runner = std::make_unique<NQumir::TLLVMRunner>(opts);
    runner->RegisterModule(dbModule, true);

    NQumir::NAst::NCore::PrintAst(std::cerr, kernelAst);

    std::string err;
    void* fnPtr = runner->CompileKernelAst(std::move(kernelAst), &err);
    if (!fnPtr) {
        throw std::runtime_error("filter kernel compilation failed: " + err);
    }

    using TFilterFn = void(*)(void*);
    auto sharedRunner = std::shared_ptr<NQumir::TLLVMRunner>(std::move(runner));

    TFilterDispatch dispatch = [fnPtr, sharedRunner](TRowSet& rowSet) {
        reinterpret_cast<TFilterFn>(fnPtr)(&rowSet);
    };

    return dispatch;
}

} // namespace NQqb
