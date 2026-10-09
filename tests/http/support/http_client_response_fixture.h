#pragma once

#include <array>
#include <concepts>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_client_redirect.h"
#include "ruvia/http/http_limits.h"

#include "test_harness.h"

namespace http_client_response_test {

using ruvia::http1_client_request_content_completion_status;
using ruvia::http1_client_request_wire_policy;
using ruvia::http1_client_response_parse_error;
using ruvia::http1_client_response_parse_result;
using ruvia::http1_client_response_parser;
using ruvia::http1_close_policy;
using ruvia::http1_parsed_client_response_head;
using ruvia::http_client_request_content_signal;
using ruvia::http_client_response_head;
using ruvia::http_protocol_version;

inline http1_client_response_parse_result parse_wire(std::string_view method, std::string_view wire,
    http1_close_policy close_policy = http1_close_policy::allow_reuse,
    std::span<const ruvia::http_header_view> request_headers = {},
    std::pmr::memory_resource* resource = nullptr) {
    std::array<char, 2048> request_head;
    const auto origin = ruvia::http_origin_view::https({.host_ = "example.test"});
    ruvia::http_client_request_view request;
    request.method_ = method;
    request.headers_ = request_headers;
    const auto prepared_result =
        method == "CONNECT"
            ? ruvia::http1_client_request_writer().prepare_connect(origin, request_headers, request_head,
                  http1_client_request_wire_policy{.close_policy_ = close_policy})
            : ruvia::http1_client_request_writer().prepare(origin, request, request_head,
                  http1_client_request_wire_policy{.close_policy_ = close_policy});
    const auto* prepared = prepared_result.prepared();
    if (prepared == nullptr) {
        throw std::runtime_error("test request could not be prepared");
    }
    auto parser = http1_client_response_parser(prepared->exchange_state(), {.resource_ = resource});
    return parser.parse(wire);
}

inline http1_client_response_parse_result parse_result(std::string_view method,
    std::string_view header_section, http1_close_policy close_policy = http1_close_policy::allow_reuse,
    std::span<const ruvia::http_header_view> request_headers = {},
    std::pmr::memory_resource* resource = nullptr) {
    std::string wire(header_section);
    wire.append("\r\n\r\n");
    return parse_wire(method, wire, close_policy, request_headers, resource);
}

inline http1_parsed_client_response_head parse_head(std::string_view method,
    std::string_view header_section, http1_close_policy close_policy = http1_close_policy::allow_reuse,
    std::span<const ruvia::http_header_view> request_headers = {}) {
    auto result_value = parse_result(method, header_section, close_policy, request_headers);
    auto* parsed_value = result_value.parsed();
    if (parsed_value == nullptr) {
        throw std::runtime_error("test expected a parsed HTTP/1 response head");
    }
    return std::move(*parsed_value);
}

struct parsed_response final {
    http_client_response_head head_;
};

inline parsed_response parse_response(std::string_view method, std::string_view header_section) {
    auto head = parse_head(method, header_section);
    return parsed_response{std::move(head).take_head()};
}

inline bool parse_fails(std::string_view method, std::string_view header_section,
    http1_close_policy close_policy = http1_close_policy::allow_reuse,
    std::span<const ruvia::http_header_view> request_headers = {}) {
    const auto result_value = parse_result(method, header_section, close_policy, request_headers);
    return result_value.failure() != nullptr;
}

inline const ruvia::http1_client_known_length_response& require_known_length(
    const ruvia::http1_client_response_plan& plan) {
    const auto* known_length = plan.known_length();
    if (known_length == nullptr) {
        throw std::runtime_error("test expected exact-length response framing");
    }
    return *known_length;
}

inline const ruvia::http1_client_chunked_response& require_chunked(
    const ruvia::http1_client_response_plan& plan) {
    const auto* chunked = plan.chunked();
    if (chunked == nullptr) {
        throw std::runtime_error("test expected chunked response framing");
    }
    return *chunked;
}

inline const ruvia::http1_client_close_delimited_response& require_close_delimited(
    const ruvia::http1_client_response_plan& plan) {
    const auto* close_delimited = plan.close_delimited();
    if (close_delimited == nullptr) {
        throw std::runtime_error("test expected close-delimited response framing");
    }
    return *close_delimited;
}

inline const ruvia::http1_client_response_without_content& require_without_content(
    const ruvia::http1_client_response_plan& plan) {
    const auto* without_content = plan.without_content();
    if (without_content == nullptr) {
        throw std::runtime_error("test expected a final response without content");
    }
    return *without_content;
}

inline const ruvia::http1_client_response_with_zero_content& require_zero_content(
    const ruvia::http1_client_response_plan& plan) {
    const auto* zero_content = plan.zero_content();
    if (zero_content == nullptr) {
        throw std::runtime_error("test expected a response with framed zero content");
    }
    return *zero_content;
}

inline std::size_t active_plan_alternative_count(const ruvia::http1_client_response_plan& plan) noexcept {
    return static_cast<std::size_t>(plan.informational() != nullptr) +
           static_cast<std::size_t>(plan.without_content() != nullptr) +
           static_cast<std::size_t>(plan.zero_content() != nullptr) +
           static_cast<std::size_t>(plan.known_length() != nullptr) +
           static_cast<std::size_t>(plan.chunked() != nullptr) +
           static_cast<std::size_t>(plan.close_delimited() != nullptr) +
           static_cast<std::size_t>(plan.connect_tunnel() != nullptr) +
           static_cast<std::size_t>(plan.protocol_upgrade() != nullptr);
}

inline http1_client_response_parse_error parse_failure_error(std::string_view method,
    std::string_view header_section, std::span<const ruvia::http_header_view> request_headers = {}) {
    const auto result_value =
        parse_result(method, header_section, http1_close_policy::allow_reuse, request_headers);
    const auto* failure = result_value.failure();
    if (failure == nullptr) {
        throw std::runtime_error("test expected an HTTP/1 response parse failure");
    }
    return failure->error();
}

class counting_memory_resource final : public std::pmr::memory_resource {
public:
    counting_memory_resource() noexcept
        : upstream_(std::pmr::get_default_resource()) {}

    [[nodiscard]] std::size_t allocation_count() const noexcept {
        return allocation_count_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocation_count_;
        return upstream_->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        upstream_->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    std::size_t allocation_count_{0};
};

}  // namespace http_client_response_test

using namespace http_client_response_test;  // NOLINT(google-build-using-namespace)
