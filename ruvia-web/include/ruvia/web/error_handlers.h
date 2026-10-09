#pragma once

#include <concepts>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>

#include "ruvia/core/task.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/detail/callback.h"
#include "ruvia/web/detail/callback_ref.h"
#include "ruvia/web/error.h"

namespace ruvia {

class context;

namespace detail {
using http_error_handler_ref_type = callback_ref_type<task<http_response>(context&, http_error_info)>;
using http_not_found_handler_ref_type = callback_ref_type<task<http_response>(context&)>;
}  // namespace detail

// Answers a request that failed. Accepts a plain function -- on_error(&handler)
// -- and equally a lambda or any other callable, including one that captures
// the logger, config, or metrics sink the handler needs. A self-contained
// callable is owned by the handler value; references captured by that callable
// must still outlive the registered handler.
using http_error_handler_type = detail::callback<task<http_response>(context&, http_error_info)>;

// Answers a request that matched no route.
using http_not_found_handler_type = detail::callback<task<http_response>(context&)>;

struct scoped_error_handler_options final {
    std::string prefix_{};
    http_error_handler_type handler_{nullptr};
};

struct scoped_not_found_handler_options final {
    std::string prefix_{};
    http_not_found_handler_type handler_{nullptr};
};

static_assert(sizeof(http_error_handler_type) == 5 * sizeof(void*));
static_assert(sizeof(http_not_found_handler_type) == 5 * sizeof(void*));

}  // namespace ruvia
