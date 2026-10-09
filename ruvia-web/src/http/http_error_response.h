#pragma once

#include <memory_resource>

#include "ruvia/core/task.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/error_handlers.h"

namespace ruvia::detail {

[[nodiscard]] http_response make_default_error_response(
    std::pmr::memory_resource* resource, http_error_info error);

[[nodiscard]] task<http_response> invoke_error_handler(
    context& context_value, http_error_info error, http_error_handler_ref_type handler);

}  // namespace ruvia::detail
