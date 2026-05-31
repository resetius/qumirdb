#include <qdb/kernel/gen.h>
#include <qdb/ops/parse_kernel.h>

namespace NQqb {
namespace NKernel {

std::string PtrTypeToCoreStr(const NQumir::NAst::TTypePtr& type) {
    return "<ptr " + NInternal::TypeToCoreStr(type) + ">";
}

std::string SubstFieldsWithIndex(
    const std::string& expr,
    const std::unordered_set<std::string>& fieldNames)
{
    std::string result;
    std::string token;

    auto flush = [&]() {
        if (!token.empty()) {
            if (fieldNames.count(token)) {
                result += "(index i " + token + ")";
            } else {
                result += token;
            }
            token.clear();
        }
    };

    for (char c : expr) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '(' || c == ')') {
            flush();
            result += c;
        } else {
            token += c;
        }
    }
    flush();
    return result;
}

std::string GenFilterKernelSource(
    const NQumir::NAst::TStructType& inputType,
    const std::string& predicate)
{
    std::unordered_set<std::string> fieldNames;
    for (const auto& [name, _] : inputType.Fields) {
        fieldNames.insert(name);
    }
    std::string indexedPred = SubstFieldsWithIndex(predicate, fieldNames);

    std::string src;
    src += "(block ";
    src += "(fun <kernel> void (";
    src += "(var ctx <ref <struct ";
    src += "(n i64) ";
    src += "(selection <ptr bool>) ";
    for (const auto& [name, type] : inputType.Fields) {
        src += "(" + name + " " + PtrTypeToCoreStr(type) + ") ";
    }
    src += ">>) ";
    src += ") () ";             // close params, empty attrs
    src += "(block ";           // fun body
    src += "(var n i64) ";
    src += "(= n (field n ctx)) ";
    src += "(var selection <ptr bool>) ";
    src += "(= selection (field selection ctx)) ";
    for (const auto& [name, type] : inputType.Fields) {
        src += "(var " + name + " " + PtrTypeToCoreStr(type) + ") ";
        src += "(= " + name + " (field " + name + " ctx)) ";
    }
    src += "(var i i64) ";
    src += "(= i (: 0 i64)) ";
    src += "(while (< i n) ";   // while
    src += "(block ";           // while body
    src += "(= selection [i] " + indexedPred + ") ";
    src += "(= i (+ i (: 1 i64)))";
    src += ")";                 // close while body
    src += ")";                 // close while
    src += ")";                 // close fun body
    src += ")";                 // close fun
    src += ")";                 // close outer block
    return src;
}

} // namespace NKernel
} // namespace NQqb
