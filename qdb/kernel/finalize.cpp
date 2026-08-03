#include <qdb/kernel/finalize.h>

#include <qdb/kernel/compiler.h>

#include <cstdlib>
#include <ostream>
#include <stdexcept>

namespace NQdb {

void JitFinalizeKernels(
    std::span<TGeneratedKernel> kernels,
    std::ostream* diagnostics)
{
    const char* cacheDirEnv = std::getenv("QDB_JIT_CACHE_DIR");
    const std::string cacheDir = (cacheDirEnv && *cacheDirEnv) ? cacheDirEnv : "";

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
        std::unordered_map<std::string, void*> fns;
        if (!cacheDir.empty()) {
            auto linked = CompileKernelAstCached(
                *runner, kernel.Ast, kernel.Entrypoints, cacheDir, &error);
            fns = std::move(linked.Entries);
            kernel.Slot->JitLifetime = std::move(linked.Lifetime);
        } else {
            fns = CompileKernelAst(*runner, kernel.Ast, kernel.Entrypoints, &error);
        }
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
