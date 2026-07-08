#include <qdb/kernel/finalize.h>

#include <qdb/kernel/compiler.h>

#include <ostream>
#include <stdexcept>

namespace NQdb {

void JitFinalizeKernels(
    std::span<TGeneratedKernel> kernels,
    std::ostream* diagnostics)
{
    for (auto& kernel : kernels) {
        if (!kernel.Slot) {
            throw std::logic_error(
                "JitFinalizeKernels: kernel '" + kernel.Name + "' has no slot");
        }
        if (kernel.Slot->Runner) {
            continue; // already bound
        }
        if (diagnostics) {
            *diagnostics << "\n========== RUNTIME FINALIZE: " << kernel.Name
                         << " (" << kernel.Stage << ") ==========\n";
        }

        auto options = KernelRunnerOptions();
        options.NativeCode = true;
        options.EnablePerfJitEventListener = true;
        options.PrintIr = diagnostics != nullptr;
        options.PrintLlvm = diagnostics != nullptr;
        auto runner = std::make_shared<NQumir::TLLVMRunner>(std::move(options));

        std::string error;
        auto fns = CompileKernelAst(*runner, kernel.Ast, kernel.Entrypoints, &error);
        if (diagnostics) {
            *diagnostics << "========== END RUNTIME FINALIZE ==========\n";
        }
        for (size_t i = 0; i < kernel.Entrypoints.size(); ++i) {
            auto it = fns.find(kernel.Entrypoints[i]);
            if (it == fns.end() || !it->second) {
                throw std::runtime_error(
                    "kernel '" + kernel.Name + "' entry '" +
                    kernel.Entrypoints[i] + "' compilation failed: " + error);
            }
            kernel.Slot->Fns[i] = it->second;
        }
        kernel.Slot->Runner = std::move(runner);
    }
}

} // namespace NQdb
