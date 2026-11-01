#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/http1_client_exchange_state.h"
#include "ruvia/http/http1_request_connection_plan.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"

namespace ruvia {

// One immutable wire policy for request preparation. Expect is writer-owned so
// the emitted field and the resulting content gate cannot disagree. How long an
// I/O runtime waits before releasing a continue-gated body remains runtime
// policy, not an HTTP message-model setting.
struct http1_client_request_wire_policy final {
    http1_close_policy close_policy_{http1_close_policy::allow_reuse};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
    http_request_field_source field_source_{http_request_field_source::generated};
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
    host_header_managed_by_writer,
    content_length_managed_by_writer,
    transfer_encoding_unsupported,
    // A Trailer field declares trailer fields (RFC 9110 section 6.6.2), which
    // only chunked request content can carry (RFC 9112 section 7.1.2).
    trailer_requires_chunked_content,
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
// take_exchange_state() moves out the independent response-side protocol facts;
// head() and content_plan() remain readable for the request write or an explicit retry.
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

    [[nodiscard]] http1_client_exchange_state take_exchange_state() && noexcept {
        return std::move(exchange_state_);
    }

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

// HTTP/1.1 direct-origin request writer. Ordinary requests are allocation-free;
// an Upgrade request owns only its offered protocol value for later 101
// validation. It validates the complete request before touching the caller's
// buffer, generates Host and exact Content-Length, and returns separate
// head/content views for writev-style I/O or Expect: 100-continue gating.
// Generated port and Content-Length tokens are shared by measurement and writing.
// Metadata text and the header descriptor array may overlap head_buffer:
// successful preparation consumes them before overwriting the emitted head.
// Borrowed content must not overlap those written bytes; it may use the unused
// tail of head_buffer and remains unchanged until the runtime sends it.
// CONNECT uses its dedicated entry so authority form cannot be confused with an
// origin-form target.
// Explicit empty content emits Content-Length: 0 without triggering TRACE's
// content prohibition or generated OPTIONS' Content-Type requirement.
class http1_client_request_writer final {
public:
    explicit http1_client_request_writer(std::pmr::memory_resource* resource = nullptr) noexcept;

    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>
    prepare(const http_origin_view& origin,
        const http_client_request_view& request, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

    // RFC 9298 HTTP/1.1 GET Upgrade. Capsule-Protocol is validated; Upgrade and
    // Connection are driver-owned, and Host remains owned by the writer.
    // RFC 9931 section 6.3 forbids sending UDP payload before the server's
    // successful protocol-upgrade response in HTTP/1.x.
    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>
    prepare_connect_udp(const http_origin_view& origin,
        borrowed_text target, std::span<const http_header_view> headers, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>
    prepare_streaming(const http_origin_view& origin,
        const http1_client_request_head_view& request, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

    // When forwarding untrusted TCP payload, RFC 9931 section 8 requires the
    // driver to wait for the successful tunnel plan before sending that payload,
    // unless close_after_response makes the request emit Connection: close.
    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>
    prepare_connect(const http_origin_view& tunnel_origin,
        std::span<const http_header_view> headers, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

    [[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>
    prepare_connect(borrowed_text authority,
        std::span<const http_header_view> headers, std::span<char> head_buffer,
        http1_client_request_wire_policy policy = {}) const;

private:
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia
