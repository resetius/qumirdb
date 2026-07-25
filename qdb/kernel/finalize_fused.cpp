#include <qdb/kernel/finalize_fused.h>

#include <qdb/kernel/compiler.h>
#include <qdb/kernel/finalize_dedup.h>

#include <qumir/parser/core/printer.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NQdb {

namespace {

using namespace NQumir::NAst;

std::string KernelPrefix(size_t index) {
    return "__qdb_k" + std::to_string(index) + "_";
}

std::string PrintDecl(const TExprPtr& expr) {
    return NCore::PrintAst(expr, NCore::TPrintOptions{.Pretty = false});
}

std::string TypeKeyString(const TTypePtr& type) {
    return NQumir::NAst::TypeKey(type);
}

void RewriteType(
    const TTypePtr& type,
    const std::unordered_map<std::string, std::string>& renames,
    std::unordered_set<const TType*>& seen)
{
    if (!type || !seen.insert(type.get()).second) {
        return;
    }

    if (auto named = TMaybeType<TNamedType>(type)) {
        auto node = named.Cast();
        if (auto it = renames.find(node->Name); it != renames.end()) {
            node->Name = it->second;
        }
        for (auto& arg : node->TypeArgs) {
            if (arg.Kind == TGenericArg::EKind::Type) {
                RewriteType(arg.Type, renames, seen);
            }
        }
        RewriteType(node->UnderlyingType, renames, seen);
    } else if (auto function = TMaybeType<TFunctionType>(type)) {
        auto node = function.Cast();
        for (auto& param : node->ParamTypes) {
            RewriteType(param, renames, seen);
        }
        RewriteType(node->ReturnType, renames, seen);
    } else if (auto future = TMaybeType<TFutureType>(type)) {
        RewriteType(future.Cast()->ResultType, renames, seen);
    } else if (auto array = TMaybeType<TArrayType>(type)) {
        RewriteType(array.Cast()->ElementType, renames, seen);
    } else if (auto pointer = TMaybeType<TPointerType>(type)) {
        RewriteType(pointer.Cast()->PointeeType, renames, seen);
    } else if (auto reference = TMaybeType<TReferenceType>(type)) {
        RewriteType(reference.Cast()->ReferencedType, renames, seen);
    } else if (auto structure = TMaybeType<TStructType>(type)) {
        for (auto& field : structure.Cast()->Fields) {
            RewriteType(field.second, renames, seen);
        }
    }
}

void RewriteExprTypes(
    const TExprPtr& expr,
    const std::unordered_map<std::string, std::string>& renames,
    std::unordered_set<const TType*>& seen)
{
    if (!expr) {
        return;
    }

    RewriteType(expr->Type, renames, seen);
    if (auto fun = TMaybeNode<TFunDecl>(expr)) {
        auto node = fun.Cast();
        RewriteType(node->RetType, renames, seen);
        for (auto& generic : node->GenericParams) {
            RewriteType(generic.ValueType, renames, seen);
        }
    } else if (auto typeDecl = TMaybeNode<TTypeDeclStmt>(expr)) {
        for (auto& generic : typeDecl.Cast()->GenericParams) {
            RewriteType(generic.ValueType, renames, seen);
        }
    }

    for (auto* child : expr->MutableChildren()) {
        RewriteExprTypes(*child, renames, seen);
    }
}

void RewriteExprTypes(
    const TExprPtr& expr,
    const std::unordered_map<std::string, std::string>& renames)
{
    std::unordered_set<const TType*> seen;
    RewriteExprTypes(expr, renames, seen);
}

void RewriteCalls(
    const TExprPtr& expr,
    const std::unordered_map<std::string, std::string>& renames)
{
    if (!expr) {
        return;
    }

    if (auto call = TMaybeNode<TCallExpr>(expr)) {
        if (auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee)) {
            if (auto it = renames.find(callee.Cast()->Name); it != renames.end()) {
                callee.Cast()->Name = it->second;
            }
        }
    }

    for (auto* child : expr->MutableChildren()) {
        RewriteCalls(*child, renames);
    }
}

void CollectLocalCalls(
    const TExprPtr& expr,
    const std::unordered_set<std::string>& localNames,
    std::unordered_set<std::string>& calls)
{
    if (!expr) {
        return;
    }

    if (auto call = TMaybeNode<TCallExpr>(expr)) {
        if (auto callee = TMaybeNode<TIdentExpr>(call.Cast()->Callee)) {
            const auto& name = callee.Cast()->Name;
            if (localNames.contains(name)) {
                calls.insert(name);
            }
        }
    }

    for (const auto& child : expr->Children()) {
        CollectLocalCalls(child, localNames, calls);
    }
}

std::string FunctionSignatureKey(const TFunDecl& fun) {
    std::string key;
    key += "generics(";
    for (const auto& generic : fun.GenericParams) {
        key += generic.Name;
        key += ':';
        key += generic.Kind == TGenericParam::EKind::Type ? "type" : "value";
        key += ':';
        key += TypeKeyString(generic.ValueType);
        key += ';';
    }
    key += ")params(";
    for (const auto& param : fun.Params) {
        key += TypeKeyString(param->Type);
        key += ';';
    }
    key += ")ret(";
    key += TypeKeyString(fun.RetType);
    key += ")attrs(";
    key += fun.MangledName;
    key += ';';
    key += fun.OperatorName.value_or("");
    key += ';';
    key += fun.LiteralSuffix.value_or("");
    key += ';';
    key += fun.RequireArgsMaterialization ? "materialize" : "";
    key += ')';
    return key;
}

std::string TypeDeclName(const TTypeDeclStmt& typeDecl) {
    if (auto named = TMaybeType<TNamedType>(typeDecl.Type)) {
        return named.Cast()->Name;
    }
    return {};
}

struct TUniqueKernel {
    TGeneratedKernel* Kernel = nullptr;
    std::vector<std::string> FusedEntrypoints;
    std::vector<void*> Fns;
};

struct TFusedKernelStorage {
    TExprPtr Program;
    std::vector<std::shared_ptr<void>> KernelStorage;
};

class TFusedProgramBuilder {
public:
    std::vector<std::string> AddKernel(TGeneratedKernel& kernel, size_t index) {
        auto block = TMaybeNode<TBlockExpr>(kernel.Ast);
        if (!block) {
            throw std::runtime_error(
                "kernel '" + kernel.Name + "' AST is not a top-level block");
        }

        const std::string prefix = KernelPrefix(index);
        std::unordered_map<std::string, std::string> typeRenames;
        std::unordered_set<TExpr*> includeTypes;
        std::unordered_set<TExpr*> includeFunctions;

        DecideTypeNames(*block.Cast(), prefix, typeRenames);
        if (!typeRenames.empty()) {
            RewriteExprTypes(kernel.Ast, typeRenames);
        }
        IncludeTypeDecls(*block.Cast(), includeTypes);

        std::unordered_map<std::string, std::string> functionRenames;
        DecideFunctions(*block.Cast(), prefix, functionRenames, includeFunctions);
        if (!functionRenames.empty()) {
            RewriteFunctionDeclNames(*block.Cast(), functionRenames);
            RewriteCalls(kernel.Ast, functionRenames);
        }

        AppendIncludedDecls(*block.Cast(), includeTypes, includeFunctions);

        std::vector<std::string> result;
        result.reserve(kernel.Entrypoints.size());
        for (const auto& entrypoint : kernel.Entrypoints) {
            if (auto it = functionRenames.find(entrypoint); it != functionRenames.end()) {
                result.push_back(it->second);
            } else {
                result.push_back(entrypoint);
            }
        }
        return result;
    }

    TExprPtr Build() {
        std::vector<TExprPtr> stmts;
        stmts.reserve(TypeDecls_.size() + FunctionDecls_.size());
        for (auto& stmt : TypeDecls_) {
            stmts.push_back(std::move(stmt));
        }
        for (auto& stmt : FunctionDecls_) {
            stmts.push_back(std::move(stmt));
        }
        return std::make_shared<TBlockExpr>(NQumir::TLocation{}, std::move(stmts));
    }

    size_t TypeDeclCount() const {
        return TypeDecls_.size();
    }

    size_t FunctionDeclCount() const {
        return FunctionDecls_.size();
    }

private:
    struct TTypeVariant {
        std::string Key;
        std::string OutputName;
    };

    void DecideTypeNames(
        TBlockExpr& block,
        const std::string& prefix,
        std::unordered_map<std::string, std::string>& renames)
    {
        for (const auto& stmt : block.Stmts) {
            auto typeDecl = TMaybeNode<TTypeDeclStmt>(stmt);
            if (!typeDecl) {
                continue;
            }

            const std::string name = TypeDeclName(*typeDecl.Cast());
            if (name.empty()) {
                continue;
            }

            const std::string key = PrintDecl(stmt);
            auto& variants = TypeVariants_[name];
            auto existing = std::find_if(
                variants.begin(), variants.end(),
                [&](const TTypeVariant& variant) {
                    return variant.Key == key;
                });
            if (existing != variants.end()) {
                if (existing->OutputName != name) {
                    renames[name] = existing->OutputName;
                }
                continue;
            }

            const std::string outputName = variants.empty()
                ? name
                : prefix + name;
            variants.push_back({
                .Key = key,
                .OutputName = outputName,
            });
            if (outputName != name) {
                renames[name] = outputName;
            }
        }
    }

    void IncludeTypeDecls(
        TBlockExpr& block,
        std::unordered_set<TExpr*>& includeTypes)
    {
        for (const auto& stmt : block.Stmts) {
            auto typeDecl = TMaybeNode<TTypeDeclStmt>(stmt);
            if (!typeDecl) {
                continue;
            }

            const std::string name = TypeDeclName(*typeDecl.Cast());
            if (name.empty()) {
                throw std::runtime_error("top-level type declaration has no named type");
            }

            const std::string key = PrintDecl(stmt);
            auto& known = IncludedTypeDecls_[name];
            if (known.insert(key).second) {
                includeTypes.insert(stmt.get());
            }
        }
    }

    void DecideFunctions(
        TBlockExpr& block,
        const std::string& prefix,
        std::unordered_map<std::string, std::string>& renames,
        std::unordered_set<TExpr*>& includeFunctions)
    {
        std::vector<std::string> names;
        std::unordered_map<std::string, std::vector<std::shared_ptr<TFunDecl>>> groups;
        for (const auto& stmt : block.Stmts) {
            auto fun = TMaybeNode<TFunDecl>(stmt);
            if (!fun) {
                continue;
            }
            const std::string name = fun.Cast()->Name;
            if (!groups.contains(name)) {
                names.push_back(name);
            }
            groups[name].push_back(fun.Cast());
        }

        auto renameNames = FunctionNamesToRename(names, groups);
        for (const auto& name : names) {
            auto& functions = groups[name];
            if (renameNames.contains(name)) {
                renames[name] = prefix + name;
                for (const auto& fun : functions) {
                    includeFunctions.insert(fun.get());
                }
                continue;
            }

            for (const auto& fun : functions) {
                const std::string signature = FunctionSignatureKey(*fun);
                const std::string key = PrintDecl(fun);
                auto& knownBySignature = Functions_[name];
                auto known = knownBySignature.find(signature);
                if (known == knownBySignature.end()) {
                    knownBySignature.emplace(signature, key);
                    includeFunctions.insert(fun.get());
                } else if (known->second != key) {
                    throw std::runtime_error(
                        "function conflict was not renamed: " + name);
                }
            }
        }
    }

    std::unordered_set<std::string> FunctionNamesToRename(
        const std::vector<std::string>& names,
        const std::unordered_map<std::string, std::vector<std::shared_ptr<TFunDecl>>>& groups) const
    {
        std::unordered_set<std::string> localNames(names.begin(), names.end());
        std::unordered_map<std::string, std::unordered_set<std::string>> callsByName;
        for (const auto& name : names) {
            auto& calls = callsByName[name];
            for (const auto& fun : groups.at(name)) {
                CollectLocalCalls(fun, localNames, calls);
            }
        }

        std::unordered_set<std::string> renameNames;
        for (const auto& name : names) {
            if (MustRenameFunctionGroup(name, groups.at(name))) {
                renameNames.insert(name);
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& name : names) {
                if (renameNames.contains(name)) {
                    continue;
                }
                const auto& calls = callsByName.at(name);
                if (std::ranges::any_of(calls, [&](const std::string& callee) {
                    return renameNames.contains(callee);
                })) {
                    renameNames.insert(name);
                    changed = true;
                }
            }
        }

        return renameNames;
    }

    bool MustRenameFunctionGroup(
        const std::string& name,
        const std::vector<std::shared_ptr<TFunDecl>>& functions) const
    {
        std::unordered_map<std::string, std::string> local;
        for (const auto& fun : functions) {
            const std::string signature = FunctionSignatureKey(*fun);
            const std::string key = PrintDecl(fun);
            auto localKnown = local.find(signature);
            if (localKnown != local.end() && localKnown->second != key) {
                throw std::runtime_error(
                    "one kernel has conflicting overloads for function '" + name + "'");
            }
            local.emplace(signature, key);

            auto byName = Functions_.find(name);
            if (byName == Functions_.end()) {
                continue;
            }
            auto bySignature = byName->second.find(signature);
            if (bySignature != byName->second.end() && bySignature->second != key) {
                return true;
            }
        }
        return false;
    }

    void RewriteFunctionDeclNames(
        TBlockExpr& block,
        const std::unordered_map<std::string, std::string>& renames)
    {
        for (const auto& stmt : block.Stmts) {
            auto fun = TMaybeNode<TFunDecl>(stmt);
            if (!fun) {
                continue;
            }
            if (auto it = renames.find(fun.Cast()->Name); it != renames.end()) {
                fun.Cast()->Name = it->second;
            }
        }
    }

    void AppendIncludedDecls(
        TBlockExpr& block,
        const std::unordered_set<TExpr*>& includeTypes,
        const std::unordered_set<TExpr*>& includeFunctions)
    {
        for (const auto& stmt : block.Stmts) {
            if (TMaybeNode<TUseExpr>(stmt)) {
                continue;
            }
            if (TMaybeNode<TTypeDeclStmt>(stmt)) {
                if (includeTypes.contains(stmt.get())) {
                    TypeDecls_.push_back(stmt);
                }
            } else if (TMaybeNode<TFunDecl>(stmt)) {
                if (includeFunctions.contains(stmt.get())) {
                    FunctionDecls_.push_back(stmt);
                }
            } else {
                throw std::runtime_error(
                    "unsupported top-level kernel AST node '" +
                    std::string(stmt->NodeName()) + "'");
            }
        }
    }

    std::vector<TExprPtr> TypeDecls_;
    std::vector<TExprPtr> FunctionDecls_;
    std::unordered_map<std::string, std::vector<TTypeVariant>> TypeVariants_;
    std::unordered_map<std::string, std::unordered_set<std::string>> IncludedTypeDecls_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> Functions_;
};

} // namespace

void JitFinalizeKernelsFused(
    std::span<TGeneratedKernel> kernels,
    std::ostream* diagnostics)
{
    std::vector<TUniqueKernel> unique;
    std::vector<std::pair<TGeneratedKernel*, size_t>> bindings;
    std::unordered_map<std::string, size_t> uniqueByKey;

    for (auto& kernel : kernels) {
        if (!kernel.Slot) {
            throw std::logic_error(
                "JitFinalizeKernels: kernel '" + kernel.Name + "' has no slot");
        }
        if (kernel.Slot->Runner) {
            continue; // already bound
        }

        std::string key = MakeKernelDedupKey(kernel);
        auto [it, inserted] = uniqueByKey.emplace(std::move(key), unique.size());
        if (inserted) {
            unique.push_back(TUniqueKernel{.Kernel = &kernel});
        }
        bindings.emplace_back(&kernel, it->second);
    }

    if (unique.empty()) {
        return;
    }

    TFusedProgramBuilder builder;
    std::vector<std::string> entrypoints;
    std::unordered_set<std::string> seenEntrypoints;
    for (size_t i = 0; i < unique.size(); ++i) {
        unique[i].FusedEntrypoints = builder.AddKernel(*unique[i].Kernel, i);
        for (const auto& entrypoint : unique[i].FusedEntrypoints) {
            if (seenEntrypoints.insert(entrypoint).second) {
                entrypoints.push_back(entrypoint);
            }
        }
    }

    if (diagnostics) {
        *diagnostics << "\n========== RUNTIME FUSED FINALIZE ==========\n"
                     << "[kernel-fusion] kernels: "
                     << bindings.size()
                     << ", unique kernels: "
                     << unique.size()
                     << ", type decls: "
                     << builder.TypeDeclCount()
                     << ", function decls: "
                     << builder.FunctionDeclCount()
                     << ", entrypoints: "
                     << entrypoints.size()
                     << "\n";
    }

    auto options = KernelRunnerOptions();
    options.NativeCode = true;
    options.EnablePerfJitEventListener = true;
    options.PrintIr = diagnostics != nullptr;
    options.PrintLlvm = diagnostics != nullptr;
    auto runner = std::make_shared<NQumir::TLLVMRunner>(std::move(options));
    auto storage = std::make_shared<TFusedKernelStorage>();
    storage->KernelStorage.reserve(bindings.size());
    for (const auto& [kernel, uniqueIndex] : bindings) {
        (void)uniqueIndex;
        if (kernel->Storage) {
            storage->KernelStorage.push_back(kernel->Storage);
        }
    }
    storage->Program = builder.Build();

    std::string error;
    auto fns = CompileKernelAst(*runner, storage->Program, entrypoints, &error);
    if (diagnostics) {
        *diagnostics << "========== END RUNTIME FUSED FINALIZE ==========\n";
    }

    for (auto& item : unique) {
        item.Fns.resize(item.FusedEntrypoints.size(), nullptr);
        for (size_t i = 0; i < item.FusedEntrypoints.size(); ++i) {
            const auto& entrypoint = item.FusedEntrypoints[i];
            auto it = fns.find(entrypoint);
            if (it == fns.end() || !it->second) {
                throw std::runtime_error(
                    "kernel '" + item.Kernel->Name + "' entry '" +
                    item.Kernel->Entrypoints[i] + "' compilation failed: " + error);
            }
            item.Fns[i] = it->second;
        }
    }

    for (const auto& [kernel, uniqueIndex] : bindings) {
        kernel->Slot->Fns = unique[uniqueIndex].Fns;
        kernel->Slot->Runner = runner;
        kernel->Storage = storage;
    }
}

} // namespace NQdb
