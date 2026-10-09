#pragma once

#include "ruvia/web/server_config.h"

namespace ruvia::detail {

struct access_log_record_access final {
    [[nodiscard]] static constexpr access_log_record make(const http_request& request,
        std::string_view remote_address, http_status_code status,
        std::uint64_t duration_micros) noexcept {
        return access_log_record(request, remote_address, status, duration_micros);
    }
};

}  // namespace ruvia::detail
