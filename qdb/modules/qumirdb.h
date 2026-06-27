#pragma once

#include <qumir/modules/module.h>

namespace NQumir {
namespace NRegistry {

const std::vector<TExternalType>& QumirDbExternalTypes();
void EnsureQumirDbRuntimeSymbolsLinked();

} // namespace NRegistry
} // namespace NQumir
