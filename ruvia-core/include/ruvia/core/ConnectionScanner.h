#pragma once

#include "ruvia/core/detail/io/ConnectionScanner.h"

namespace ruvia {

// Worker-owned connection activity tracking used by protocol runtimes.
using ConnectionScanner = detail::ConnectionScanner;
using ConnectionScannerOptions = detail::ConnectionScannerOptions;

}  // namespace ruvia
