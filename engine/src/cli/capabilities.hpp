#pragma once

#include <ostream>

namespace getnative::cli {

// Writes the schema_version=2 capability envelope. analysis_available is
// true inside a worker session (analyze exists there), false for the
// one-shot CLI transport.
void write_capabilities(std::ostream &output, bool analysis_available);

} // namespace getnative::cli
