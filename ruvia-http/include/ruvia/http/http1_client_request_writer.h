#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http1_client_exchange_state.h"
#include "ruvia/http/http1_close_policy.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"

namespace ruvia {

class http1_client_response_parser;

// One immutable wire policy for request preparation. Expect is writer-owned so
// the emitted field and the resulting content gate cannot disagree. How long an
// I/O runtime waits before releasing a continue-gated body remains runtime
// policy, not an HTTP message-model setting.
struct http1_client_request_wire_policy final {
    http1_close_policy close_policy_{http1_close_policy::allow_reuse};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
};

namespace detail {

struct http1_client_request_prepare_result_access;

}  // namespace detail

struct http1_client_request_head_view final {
    borrowed_text method_{"GET"};
    borrowed_text target_{"/"};
    std::span<const http_header_view> headers_{};
    // A value sends Content-Length; absent sends Transfer-Encoding: chunked.
    std::optional<std::uint64_t> content_length_{};
};

class http1_client_streaming_request_content final {
public:
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept {
        return length_;
    }
    [[nodiscard]] bool continue_gated() const noexcept {
        return gated_;
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;
    http1_client_streaming_request_content(std::optional<std::uint64_t> length, bool gated) noexcept
        : length_(length),
          gated_(gated) {}
    std::optional<std::uint64_t> length_;
    bool gated_;
};

class http1_client_request_without_content final {
private:
    friend struct detail::http1_client_request_prepare_result_access;

    constexpr http1_client_request_without_content() noexcept = default;
};

class http1_client_immediate_request_content final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit constexpr http1_client_immediate_request_content(std::string_view bytes_value) noexcept
        : bytes_(bytes_value) {}

    std::string_view bytes_;
};

class http1_client_continue_gated_request_content final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit constexpr http1_client_continue_gated_request_content(std::string_view bytes_value) noexcept
        : bytes_(bytes_value) {}

    std::string_view bytes_;
};

// Immutable outbound content contract. Continue-gated content means the writer
// generated Expect: 100-continue and the I/O owner must send the head separately;
// it may release those bytes after 100 Continue or according to its finite wait
// policy. Immediate content includes explicit empty content (Content-Length: 0).
// Payload exists only on the two alternatives that actually send content.
class http1_client_request_content_plan final {
public:
    [[nodiscard]] constexpr const http1_client_request_without_content* without_content()
        const& noexcept {
        return std::get_if<http1_client_request_without_content>(&content_);
    }
    const http1_client_request_without_content* without_content() const&& = delete;

    [[nodiscard]] constexpr const http1_client_immediate_request_content* immediate() const& noexcept {
        return std::get_if<http1_client_immediate_request_content>(&content_);
    }
    const http1_client_immediate_request_content* immediate() const&& = delete;

    [[nodiscard]] constexpr const http1_client_continue_gated_request_content* continue_gated()
        const& noexcept {
        return std::get_if<http1_client_continue_gated_request_content>(&content_);
    }
    const http1_client_continue_gated_request_content* continue_gated() const&& = delete;

    [[nodiscard]] constexpr const http1_client_streaming_request_content* streaming() const& noexcept {
        return std::get_if<http1_client_streaming_request_content>(&content_);
    }
    const http1_client_streaming_request_content* streaming() const&& = delete;

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit http1_client_request_content_plan(http1_client_streaming_request_content content) noexcept
        : content_(content) {}
    using content_type = std::variant<http1_client_request_without_content,
        http1_client_immediate_request_content, http1_client_continue_gated_request_content, http1_client_streaming_request_content>;

    explicit constexpr http1_client_request_content_plan(
        http1_client_request_without_content content) noexcept
        : content_(content) {}

    explicit constexpr http1_client_request_content_plan(
        http1_client_immediate_request_content content) noexcept
        : content_(content) {}

    explicit constexpr http1_client_request_content_plan(
        http1_client_continue_gated_request_content content) noexcept
        : content_(content) {}

    content_type content_;
};

enum class http1_client_request_prepare_error : std::uint8_t {
    invalid_method,
    invalid_target,
    connect_requires_dedicated_entry,
    invalid_connect_origin,
    invalid_header,
    too_many_headers,
    host_header_managed_by_writer,
    content_length_managed_by_writer,
    transfer_encoding_unsupported,
    trailer_section_unsupported,
    expect_header_managed_by_writer,
    invalid_connection,
    invalid_upgrade,
    upgrade_connection_option_required,
    te_connection_option_required,
    expectation_without_content,
    content_forbidden_for_method,
    options_content_type_required,
    header_too_large,
    invalid_close_policy,
    invalid_expectation,
};

[[nodiscard]] std::string_view http1_client_request_prepare_error_message(
    http1_client_request_prepare_error error) noexcept;

class http1_client_request_buffer_too_small final {
public:
    [[nodiscard]] constexpr std::size_t required_head_bytes() const noexcept {
        return required_head_bytes_;
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit constexpr http1_client_request_buffer_too_small(std::size_t required_head_bytes) noexcept
        : required_head_bytes_(required_head_bytes) {}

    std::size_t required_head_bytes_;
};

// Transactionally prepared scatter-gather request. `head()` points into the
// caller-provided output buffer and the active immediate/continue-gated
// alternative points into the request's borrowed content; those sources must
// remain alive and unchanged until sent.
// exchange_state() returns independent response-side protocol facts; head() and
// content_plan() remain readable for the request write or an explicit retry.
class prepared_http1_client_request final {
public:
    [[nodiscard]] constexpr std::string_view head() const& noexcept {
        return head_;
    }
    [[nodiscard]] constexpr std::string_view head() const&& = delete;

    [[nodiscard]] constexpr const http1_client_request_content_plan& content_plan() const& noexcept {
        return content_plan_;
    }
    [[nodiscard]] constexpr const http1_client_request_content_plan& content_plan() const&& = delete;

    [[nodiscard]] http1_client_exchange_state exchange_state() const& {
        return http1_client_exchange_state(
            exchange_state_, exchange_state_.offered_upgrade_protocols_.get_allocator().resource());
    }
    http1_client_exchange_state exchange_state() const&& = delete;

private:
    friend struct detail::http1_client_request_prepare_result_access;

    prepared_http1_client_request(std::string_view head, http1_client_request_content_plan content_plan,
        http1_client_exchange_state exchange_state) noexcept
        : head_(head),
          content_plan_(content_plan),
          exchange_state_(std::move(exchange_state)) {}

    std::string_view head_;
    http1_client_request_content_plan content_plan_;
    http1_client_exchange_state exchange_state_;
};

class http1_client_request_prepare_failure final {
public:
    [[nodiscard]] constexpr http1_client_request_prepare_error error() const noexcept {
        return error_;
    }

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit constexpr http1_client_request_prepare_failure(
        http1_client_request_prepare_error error) noexcept
        : error_(error) {}

    http1_client_request_prepare_error error_;
};

class http1_client_request_prepare_result final {
public:
    [[nodiscard]] constexpr const http1_client_request_buffer_too_small* buffer_too_small()
        const& noexcept {
        return std::get_if<http1_client_request_buffer_too_small>(&state_);
    }
    const http1_client_request_buffer_too_small* buffer_too_small() const&& = delete;

    [[nodiscard]] constexpr prepared_http1_client_request* prepared() & noexcept {
        return std::get_if<prepared_http1_client_request>(&state_);
    }

    [[nodiscard]] constexpr const prepared_http1_client_request* prepared() const& noexcept {
        return std::get_if<prepared_http1_client_request>(&state_);
    }
    const prepared_http1_client_request* prepared() const&& = delete;

    [[nodiscard]] constexpr const http1_client_request_prepare_failure* failure() const& noexcept {
        return std::get_if<http1_client_request_prepare_failure>(&state_);
    }
    const http1_client_request_prepare_failure* failure() const&& = delete;

private:
    friend struct detail::http1_client_request_prepare_result_access;

    explicit constexpr http1_client_request_prepare_result(
        http1_client_request_buffer_too_small state_value) noexcept
        : state_(state_value) {}

    explicit http1_client_request_prepare_result(prepared_http1_client_request state_value) noexcept
        : state_(std::move(state_value)) {}

    explicit constexpr http1_client_request_prepare_result(
        http1_client_request_prepare_failure state_value) noexcept
        : state_(state_value) {}

    std::variant<http1_client_request_buffer_too_small, prepared_http1_client_request,
        http1_client_request_prepare_failure>
        state_;
};

// HTTP/1.1 direct-origin request writer. Ordinary requests are allocation-free;
// an Upgrade request owns only its offered protocol value for later 101
// validation. It validates the complete request before touching the caller's
// buffer, generates Host and exact Content-Length, and returns separate
// head/content views for writev-style I/O or Expect: 100-continue gating.
// CONNECT uses its dedicated entry so authority form cannot be confused with an
// origin-form target.
class http1_client_request_writer final {
public:
    struct options_type final {
        std::pmr::memory_resource* resource_{nullptr};
    };

    http1_client_request_writer() noexcept;
    explicit http1_client_request_writer(options_type options) noexcept;

    [[nodiscard]] http1_client_request_prepare_result prepare(const http_origin_view& origin,
        const http_client_request_view& request, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

    // RFC 9298 HTTP/1.1 GET Upgrade. Capsule-Protocol is validated; Upgrade and
    // Connection are driver-owned, and Host remains owned by the writer.
    [[nodiscard]] http1_client_request_prepare_result prepare_connect_udp(const http_origin_view& origin,
        borrowed_text target, std::span<const http_header_view> headers, std::span<char> head_buffer) const;

    [[nodiscard]] http1_client_request_prepare_result prepare_streaming(const http_origin_view& origin,
        const http1_client_request_head_view& request, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

    [[nodiscard]] http1_client_request_prepare_result prepare_connect(const http_origin_view& tunnel_origin,
        std::span<const http_header_view> headers, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

    [[nodiscard]] http1_client_request_prepare_result prepare_connect(borrowed_text authority,
        std::span<const http_header_view> headers, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

private:
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia
