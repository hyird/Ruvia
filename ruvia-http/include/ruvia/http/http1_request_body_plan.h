#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

#include "ruvia/http/http_expectations.h"
#include "ruvia/http/http_transfer_coding.h"

namespace ruvia {

class http1_server_request_parse_state;
class http1_server_request_parser;

// Whether the runtime consumed the complete request message before attempting
// to reuse its transport. This is a protocol lifecycle fact, not a product
// policy bool: RFC 9112 section 9.3 forbids reuse with unread request content.
enum class http1_request_body_consumption : std::uint8_t { complete,
    incomplete };

class http1_request_without_body final {
private:
    friend class http1_request_body_plan;
    constexpr http1_request_without_body() noexcept = default;
};

class http1_known_length_request_body final {
public:
    [[nodiscard]] constexpr std::size_t content_length() const noexcept {
        return content_length_;
    }

private:
    friend class http1_request_body_plan;

    explicit constexpr http1_known_length_request_body(std::size_t content_length) noexcept
        : content_length_(content_length) {}

    std::size_t content_length_;
};

class http1_chunked_request_body final {
public:
    [[nodiscard]] const http_transfer_codings& transfer_codings() const noexcept {
        return transfer_codings_;
    }

private:
    friend class http1_request_body_plan;

    explicit http1_chunked_request_body(http_transfer_codings transfer_codings)
        : transfer_codings_(std::move(transfer_codings)) {}

    http_transfer_codings transfer_codings_;
};

// Immutable framing contract produced only by the HTTP/1 parser and consumed
// by any runtime driver. Content-Length belongs only to the known-length
// alternative; transfer-coding order belongs only to the final-chunked
// alternative. A caller cannot synthesize a plan that bypasses wire validation.
class http1_request_body_plan final {
public:
    [[nodiscard]] constexpr const http1_request_without_body* without_body() const& noexcept {
        return std::get_if<http1_request_without_body>(&framing_);
    }
    [[nodiscard]] constexpr const http1_request_without_body* without_body() const&& = delete;

    [[nodiscard]] constexpr const http1_known_length_request_body* known_length() const& noexcept {
        return std::get_if<http1_known_length_request_body>(&framing_);
    }
    [[nodiscard]] constexpr const http1_known_length_request_body* known_length() const&& = delete;

    [[nodiscard]] constexpr const http1_chunked_request_body* chunked() const& noexcept {
        return std::get_if<http1_chunked_request_body>(&framing_);
    }
    [[nodiscard]] constexpr const http1_chunked_request_body* chunked() const&& = delete;

    // Chunked framing requires consuming the terminating zero chunk even when
    // the decoded content is empty.
    [[nodiscard]] constexpr bool requires_consumption() const noexcept {
        if (const auto* known = known_length()) {
            return known->content_length() != 0;
        }
        return chunked() != nullptr;
    }

    [[nodiscard]] http_request_expectations expectations() const noexcept {
        return expectations_;
    }

    [[nodiscard]] http_server_expectation_plan expectation_plan(
        http_unsupported_expectation_policy unsupported_policy) const noexcept {
        return expectations_.server_plan(requires_consumption()
                                             ? http_request_content_indication::will_follow
                                             : http_request_content_indication::no_content,
            unsupported_policy);
    }

private:
    friend class http1_server_request_parse_state;
    friend class http1_server_request_parser;

    using framing_type =
        std::variant<http1_request_without_body, http1_known_length_request_body, http1_chunked_request_body>;

    explicit http1_request_body_plan(http_request_expectations expectations) noexcept
        : http1_request_body_plan(framing_type(http1_request_without_body()), expectations) {}

    http1_request_body_plan(std::size_t content_length, http_request_expectations expectations) noexcept
        : http1_request_body_plan(framing_type(http1_known_length_request_body(content_length)), expectations) {}

    http1_request_body_plan(
        http_transfer_codings transfer_codings, http_request_expectations expectations)
        : http1_request_body_plan(framing_type(http1_chunked_request_body(std::move(transfer_codings))), expectations) {}

    http1_request_body_plan(framing_type framing, http_request_expectations expectations)
        : framing_(std::move(framing)),
          expectations_(expectations) {}

    framing_type framing_;
    http_request_expectations expectations_;
};

}  // namespace ruvia
