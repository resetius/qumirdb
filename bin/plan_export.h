#pragma once

namespace NQdb {

// `qdb --plan-export`: reads a JSON request on stdin and writes a plan/runtime
// bundle on stdout. Shares the process with the CLI so the web service and the
// tests need only one binary.
int RunPlanExport(int argc, char** argv);

} // namespace NQdb
