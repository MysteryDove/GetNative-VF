#pragma once

#include <istream>
#include <ostream>

namespace getnative::cli {

// Runs the JSON Lines worker protocol (docs/worker-protocol-v1.md) until
// shutdown or EOF. Returns the process exit code.
int run_worker(std::istream &input, std::ostream &output, std::ostream &log);

} // namespace getnative::cli
