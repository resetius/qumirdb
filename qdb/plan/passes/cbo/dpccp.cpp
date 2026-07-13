#include <qdb/plan/passes/cbo/dpccp.h>

namespace NQdb {
namespace NCbo {

TOperatorPtr DpccpJoinOrder(
    const std::vector<TOperatorPtr>& leaves,
    const std::vector<TJoinEdge>& edges)
{
    (void)leaves;
    (void)edges;
    return nullptr; // TODO: DPccp enumeration (CBO_DPCCP_PLAN.md)
}

} // namespace NCbo
} // namespace NQdb
