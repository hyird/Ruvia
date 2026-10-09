#pragma once

#include <stdexcept>

#include "ruvia/http/http3_var_int.h"
#include "ruvia/web/http3_qpack_config.h"

namespace ruvia::detail {
inline void validate_http3_qpack_config(http3_qpack_config config) {
    if (config.max_table_capacity_ > http3_var_int_max || config.max_blocked_streams_ > http3_var_int_max) {
        throw std::invalid_argument("HTTP/3 QPACK limits exceed the QUIC variable integer range");
    }
}
}  // namespace ruvia::detail
