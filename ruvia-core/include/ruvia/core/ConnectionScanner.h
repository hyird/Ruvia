#pragma once

#include "ruvia/core/ConnectionScannerOptions.h"
#include "ruvia/core/detail/io/ConnectionScanner.h"

namespace ruvia {

class ConnectionScanner final : public detail::ConnectionScanner {
public:
    using detail::ConnectionScanner::ConnectionScanner;
};

}  // namespace ruvia
