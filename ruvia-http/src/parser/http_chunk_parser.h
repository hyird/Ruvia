#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/detail/util/borrowed_view.h"

namespace ruvia::detail {

enum class http_chunk_scan_error : std::uint8_t {
    invalid_size,
    size_overflow,
    invalid_extension,
    invalid_crlf,
    invalid_trailer,
    too_large
};

class http_chunk_trailer_field final {
public:
    [[nodiscard]] constexpr std::string_view name() const& noexcept {
        return name_;
    }
    std::string_view name() const&& = delete;
    [[nodiscard]] constexpr std::string_view value() const& noexcept {
        return value_;
    }
    std::string_view value() const&& = delete;

private:
    friend class http_chunk_trailer_parse_result;
    friend class http_chunk_trailer_parser;

    constexpr http_chunk_trailer_field(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    std::string_view name_;
    std::string_view value_;
};

class http_chunk_trailer_end final {
private:
    friend class http_chunk_trailer_parse_result;
    friend class http_chunk_trailer_parser;
    constexpr http_chunk_trailer_end() noexcept = default;
};

class http_chunk_trailer_failure final {
public:
    [[nodiscard]] constexpr http_chunk_scan_error error() const noexcept {
        return error_;
    }

private:
    friend class http_chunk_trailer_parse_result;
    friend class http_chunk_trailer_parser;

    explicit constexpr http_chunk_trailer_failure(http_chunk_scan_error error) noexcept
        : error_(error) {}

    http_chunk_scan_error error_;
};

class http_chunk_trailer_parse_result final {
public:
    [[nodiscard]] const http_chunk_trailer_field* field() const& noexcept {
        return std::get_if<http_chunk_trailer_field>(&value_);
    }
    const http_chunk_trailer_field* field() const&& = delete;
    [[nodiscard]] const http_chunk_trailer_end* end() const& noexcept {
        return std::get_if<http_chunk_trailer_end>(&value_);
    }
    const http_chunk_trailer_end* end() const&& = delete;
    [[nodiscard]] const http_chunk_trailer_failure* failure() const& noexcept {
        return std::get_if<http_chunk_trailer_failure>(&value_);
    }
    const http_chunk_trailer_failure* failure() const&& = delete;

private:
    friend class http_chunk_trailer_parser;
    using value_type = std::variant<http_chunk_trailer_field, http_chunk_trailer_end, http_chunk_trailer_failure>;

    template <typename result_type>
    explicit constexpr http_chunk_trailer_parse_result(result_type result_value) noexcept
        : value_(result_value) {}

    value_type value_;
};

// Iterates a validated-or-untrusted trailer block without allocation. Field
// views borrow the block passed to the constructor and remain valid until that
// storage is mutated.
class http_chunk_trailer_parser final {
public:
    explicit constexpr http_chunk_trailer_parser(std::string_view trailers) noexcept
        : trailers_(trailers) {}

    template <http_temporary_owning_char_string trailers_type>
    explicit http_chunk_trailer_parser(trailers_type&&) = delete;

    [[nodiscard]] http_chunk_trailer_parse_result next() noexcept;

private:
    [[nodiscard]] http_chunk_trailer_parse_result fail(http_chunk_scan_error error) noexcept;

    std::string_view trailers_;
    std::size_t cursor_{0};
    std::size_t field_count_{0};
    std::optional<http_chunk_scan_error> failure_;
};

class http_chunk_scan_need_more final {
private:
    friend class http_chunk_scan_result;
    constexpr http_chunk_scan_need_more() noexcept = default;
};

class http_chunk_scan_complete final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http_chunk_scan_result;

    explicit constexpr http_chunk_scan_complete(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}

    std::size_t consumed_bytes_;
};

class http_chunk_scan_failure final {
public:
    [[nodiscard]] constexpr http_chunk_scan_error error() const noexcept {
        return error_;
    }

private:
    friend class http_chunk_scan_result;

    explicit constexpr http_chunk_scan_failure(http_chunk_scan_error error) noexcept
        : error_(error) {}

    http_chunk_scan_error error_;
};

// Whole-message chunk framing has three mutually exclusive outcomes. Only a
// complete result owns a consumed byte count; need-more and failure cannot
// accidentally expose a plausible framing boundary.
class http_chunk_scan_result final {
public:
    [[nodiscard]] const http_chunk_scan_need_more* need_more() const& noexcept {
        return std::get_if<http_chunk_scan_need_more>(&value_);
    }
    const http_chunk_scan_need_more* need_more() const&& = delete;

    [[nodiscard]] const http_chunk_scan_complete* complete() const& noexcept {
        return std::get_if<http_chunk_scan_complete>(&value_);
    }
    const http_chunk_scan_complete* complete() const&& = delete;

    [[nodiscard]] const http_chunk_scan_failure* failure() const& noexcept {
        return std::get_if<http_chunk_scan_failure>(&value_);
    }
    const http_chunk_scan_failure* failure() const&& = delete;

private:
    friend http_chunk_scan_result scan_http_chunked_body(std::string_view body) noexcept;

    using value_type = std::variant<http_chunk_scan_need_more, http_chunk_scan_complete, http_chunk_scan_failure>;

    template <typename result_type>
    explicit http_chunk_scan_result(result_type result_value) noexcept
        : value_(std::move(result_value)) {}

    [[nodiscard]] static http_chunk_scan_result make_need_more() noexcept {
        return http_chunk_scan_result(http_chunk_scan_need_more());
    }

    [[nodiscard]] static http_chunk_scan_result make_complete(std::size_t consumed_bytes) noexcept {
        return http_chunk_scan_result(http_chunk_scan_complete(consumed_bytes));
    }

    [[nodiscard]] static http_chunk_scan_result make_failure(http_chunk_scan_error error) noexcept {
        return http_chunk_scan_result(http_chunk_scan_failure(error));
    }

    value_type value_;
};

[[nodiscard]] std::optional<http_chunk_scan_error> validate_http_chunk_trailers(
    std::string_view trailers) noexcept;
// Scans one complete request body without retaining payload. Payload and total
// framing each have default_max_buffered_body_bytes budgets; individual size lines
// and encoded trailer sections are bounded by max_http_header_bytes.
[[nodiscard]] http_chunk_scan_result scan_http_chunked_body(std::string_view body) noexcept;

}  // namespace ruvia::detail
