#include <concepts>
#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/server/http_final_response_control_plan.h"
#include "ruvia/http/http_response.h"

#include "response/http_response_headers_access.h"
#include "test_harness.h"

namespace {

using ruvia::http_response;
using ruvia::detail::http1_final_response_control;
using ruvia::detail::http1_final_response_control_plan;
using ruvia::detail::http1_final_response_control_plan_error;
using ruvia::detail::http1_final_response_control_plan_failure;
using ruvia::detail::http1_final_response_control_plan_result_type;
using ruvia::detail::http2_final_response_control;
using ruvia::detail::http2_final_response_control_plan;
using ruvia::detail::http2_final_response_control_plan_error;
using ruvia::detail::http2_final_response_control_plan_failure;
using ruvia::detail::http2_final_response_control_plan_result_type;

bool is_http1_failure(const http_response& response, http1_final_response_control_plan_error error) {
    const auto result_value = http1_final_response_control_plan(response);
    return result_value.control() == nullptr && result_value.failure() != nullptr &&
           result_value.failure()->error() == error;
}

bool is_http2_failure(const http_response& response, http2_final_response_control_plan_error error) {
    const auto result_value = http2_final_response_control_plan(response);
    return result_value.control() == nullptr && result_value.failure() != nullptr &&
           result_value.failure()->error() == error;
}

void add_unchecked_header(http_response& response, std::string_view name, std::string_view value) {
    auto& headers = const_cast<ruvia::http_response_headers&>(response.headers());
    (void)ruvia::detail::http_response_headers_access::add(headers, name, value, 0);
}

}  // namespace

RUVIA_TEST(final_response_control_entry_points_own_only_their_protocol) {
    http_response http1({.resource_ = std::pmr::get_default_resource()});
    http1.header("Connection", "close");
    http1.header("Connection", "Upgrade",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    http1.header("Connection", "X-Hop",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    http1.header("Upgrade", "websocket");
    http1.header("X-Hop", "value");

    const auto http1_result = http1_final_response_control_plan(http1);
    RUVIA_CHECK(http1_result.failure() == nullptr);
    const auto* http1_control = http1_result.control();
    RUVIA_CHECK(http1_control != nullptr);
    if (http1_control != nullptr) {
        RUVIA_CHECK(http1_control->connection_options().close());
        RUVIA_CHECK(http1_control->connection_options().upgrade());
        RUVIA_CHECK(http1_control->upgrade_protocols().has_field());
        RUVIA_CHECK(http1_control->upgrade_protocols().has_protocol());
    }

    http_response http2({.resource_ = std::pmr::get_default_resource()});
    http2.header("X-Trace", "ok");
    const auto http2_result = http2_final_response_control_plan(http2);
    RUVIA_CHECK(http2_result.failure() == nullptr);
    RUVIA_CHECK(http2_result.control() != nullptr);
}

RUVIA_TEST(final_response_control_failure_never_exposes_protocol_alternative) {
    http_response invalid_connection({.resource_ = std::pmr::get_default_resource()});
    add_unchecked_header(invalid_connection, "Connection", ", close");
    RUVIA_CHECK(is_http1_failure(
        invalid_connection, http1_final_response_control_plan_error::invalid_connection_field));

    http_response invalid_upgrade({.resource_ = std::pmr::get_default_resource()});
    add_unchecked_header(invalid_upgrade, "Upgrade", "web socket");
    RUVIA_CHECK(
        is_http1_failure(invalid_upgrade, http1_final_response_control_plan_error::invalid_upgrade_field));

    http_response missing_upgrade({.resource_ = std::pmr::get_default_resource()});
    missing_upgrade.status(ruvia::http_status::upgrade_required);
    RUVIA_CHECK(
        is_http1_failure(missing_upgrade, http1_final_response_control_plan_error::upgrade_required));
    RUVIA_CHECK(
        is_http2_failure(missing_upgrade, http2_final_response_control_plan_error::upgrade_unavailable));

    http_response te_response_field({.resource_ = std::pmr::get_default_resource()});
    add_unchecked_header(te_response_field, "TE", "trailers");
    RUVIA_CHECK(
        is_http1_failure(te_response_field, http1_final_response_control_plan_error::te_field_forbidden));
}

RUVIA_TEST(final_response_control_rejects_end_to_end_connection_options) {
    for (const std::string_view option : {"content-length", "DATE", "Set-Cookie"}) {
        http_response response({.resource_ = std::pmr::get_default_resource()});
        add_unchecked_header(response, "Connection", option);
        RUVIA_CHECK(
            is_http1_failure(response, http1_final_response_control_plan_error::invalid_connection_field));
    }
}

RUVIA_TEST(final_response_control_rejects_every_http2_connection_specific_field) {
    constexpr std::pair<std::string_view, std::string_view> fields_value[] = {
        {"Connection", "close"},
        {"Keep-Alive", "timeout=5"},
        {"Proxy-Connection", "keep-alive"},
        {"TE", "trailers"},
        {"Transfer-Encoding", "chunked"},
        {"Upgrade", "websocket"},
    };
    for (const auto& [name, value] : fields_value) {
        http_response response({.resource_ = std::pmr::get_default_resource()});
        if (name == "TE") {
            add_unchecked_header(response, name, value);
        } else {
            response.header(name, value);
        }
        RUVIA_CHECK(is_http2_failure(
            response, http2_final_response_control_plan_error::connection_specific_field_forbidden));
    }
}
