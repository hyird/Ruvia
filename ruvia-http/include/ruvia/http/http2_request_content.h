#pragma once

#include <cstdint>
#include <optional>
#include <variant>

namespace ruvia {

// One outbound content contract determines both Content-Length and END_STREAM.
// Public callers and the protocol engine use this same value directly.
class http2_request_content;

class http2_request_without_content final {
private:
    friend class http2_request_content;
    constexpr http2_request_without_content() noexcept = default;
};

class http2_known_length_request_content final {
public:
    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

private:
    friend class http2_request_content;
    explicit constexpr http2_known_length_request_content(std::uint64_t length) noexcept
        : length_(length) {}
    std::uint64_t length_;
};

class http2_streaming_request_content final {
public:
    [[nodiscard]] constexpr std::optional<std::uint64_t> expected_length() const noexcept {
        return length_;
    }

private:
    friend class http2_request_content;
    explicit constexpr http2_streaming_request_content(std::optional<std::uint64_t> length) noexcept
        : length_(length) {}
    std::optional<std::uint64_t> length_{};
};

class http2_request_content final {
public:
    [[nodiscard]] static constexpr http2_request_content none() noexcept {
        return http2_request_content(http2_request_without_content());
    }
    [[nodiscard]] static constexpr http2_request_content known_length(std::uint64_t length) noexcept {
        return http2_request_content(http2_known_length_request_content(length));
    }
    [[nodiscard]] static constexpr http2_request_content streaming(std::optional<std::uint64_t> length = {}) noexcept {
        return http2_request_content(http2_streaming_request_content(length));
    }
    [[nodiscard]] constexpr const http2_request_without_content* without_content() const& noexcept {
        return std::get_if<http2_request_without_content>(&value_);
    }
    const http2_request_without_content* without_content() const&& = delete;
    [[nodiscard]] constexpr const http2_known_length_request_content* known_length_content() const& noexcept {
        return std::get_if<http2_known_length_request_content>(&value_);
    }
    const http2_known_length_request_content* known_length_content() const&& = delete;
    [[nodiscard]] constexpr const http2_streaming_request_content* streaming_content() const& noexcept {
        return std::get_if<http2_streaming_request_content>(&value_);
    }
    const http2_streaming_request_content* streaming_content() const&& = delete;

private:
    using value_type = std::variant<http2_request_without_content, http2_known_length_request_content,
        http2_streaming_request_content>;
    explicit constexpr http2_request_content(http2_request_without_content value) noexcept
        : value_(value) {}
    explicit constexpr http2_request_content(http2_known_length_request_content value) noexcept
        : value_(value) {}
    explicit constexpr http2_request_content(http2_streaming_request_content value) noexcept
        : value_(value) {}
    value_type value_;
};

}  // namespace ruvia
