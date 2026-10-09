#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <variant>

namespace ruvia::detail {

class http2_remote_content_state;

class http2_remote_content_allowed_without_length final {
public:
    [[nodiscard]] constexpr std::size_t received_bytes() const noexcept {
        return received_bytes_;
    }

private:
    friend class http2_remote_content_state;

    explicit constexpr http2_remote_content_allowed_without_length(
        std::size_t received_bytes = 0) noexcept
        : received_bytes_(received_bytes) {}

    std::size_t received_bytes_{0};
};

class http2_remote_content_allowed_known_length final {
public:
    [[nodiscard]] constexpr std::size_t declared_length() const noexcept {
        return declared_length_;
    }

    [[nodiscard]] constexpr std::size_t received_bytes() const noexcept {
        return received_bytes_;
    }

private:
    friend class http2_remote_content_state;

    explicit constexpr http2_remote_content_allowed_known_length(
        std::size_t declared_length, std::size_t received_bytes = 0) noexcept
        : declared_length_(declared_length),
          received_bytes_(received_bytes) {}

    std::size_t declared_length_;
    std::size_t received_bytes_{0};
};

class http2_remote_content_metadata_only_without_length final {
private:
    friend class http2_remote_content_state;

    constexpr http2_remote_content_metadata_only_without_length() noexcept = default;
};

class http2_remote_content_metadata_only_known_length final {
public:
    [[nodiscard]] constexpr std::size_t declared_length() const noexcept {
        return declared_length_;
    }

private:
    friend class http2_remote_content_state;

    explicit constexpr http2_remote_content_metadata_only_known_length(
        std::size_t declared_length) noexcept
        : declared_length_(declared_length) {}

    std::size_t declared_length_;
};

enum class http2_remote_content_accounting_result : std::uint8_t {
    accepted,
    counter_overflow,
    declared_length_exceeded,
    content_forbidden
};

// Content allowance, Content-Length ownership, and received-byte accounting are
// one exclusive state. HEAD/204/304 responses retain representation length
// metadata but cannot accidentally accept DATA as message content. account() is
// the only byte mutation: a rejected input leaves the active alternative intact.
class http2_remote_content_state final {
public:
    constexpr http2_remote_content_state() noexcept
        : state_(http2_remote_content_allowed_without_length()) {}

    [[nodiscard]] bool declare_known_length(std::size_t length) noexcept {
        if (const auto* known = allowed_known_length(); known != nullptr) {
            return known->declared_length() == length;
        }
        if (const auto* known = metadata_only_known_length(); known != nullptr) {
            return known->declared_length() == length;
        }
        if (const auto* allowed = allowed_without_length(); allowed != nullptr) {
            if (allowed->received_bytes() != 0) {
                return false;
            }
            state_ = state_type(http2_remote_content_allowed_known_length(length));
            return true;
        }
        if (metadata_only_without_length() != nullptr) {
            state_ = state_type(http2_remote_content_metadata_only_known_length(length));
            return true;
        }
        return false;
    }

    [[nodiscard]] bool select_metadata_only() noexcept {
        if (metadata_only_without_length() != nullptr || metadata_only_known_length() != nullptr) {
            return true;
        }
        if (const auto* allowed = allowed_without_length(); allowed != nullptr) {
            if (allowed->received_bytes() != 0) {
                return false;
            }
            state_ = state_type(http2_remote_content_metadata_only_without_length());
            return true;
        }
        if (const auto* allowed = allowed_known_length(); allowed != nullptr) {
            if (allowed->received_bytes() != 0) {
                return false;
            }
            state_ = state_type(http2_remote_content_metadata_only_known_length(allowed->declared_length()));
            return true;
        }
        return false;
    }

    [[nodiscard]] http2_remote_content_accounting_result account(std::size_t bytes_value) noexcept {
        if (auto* allowed = std::get_if<http2_remote_content_allowed_without_length>(&state_);
            allowed != nullptr) {
            if (bytes_value > std::numeric_limits<std::size_t>::max() - allowed->received_bytes_) {
                return http2_remote_content_accounting_result::counter_overflow;
            }
            allowed->received_bytes_ += bytes_value;
            return http2_remote_content_accounting_result::accepted;
        }
        if (auto* allowed = std::get_if<http2_remote_content_allowed_known_length>(&state_);
            allowed != nullptr) {
            if (bytes_value > std::numeric_limits<std::size_t>::max() - allowed->received_bytes_) {
                return http2_remote_content_accounting_result::counter_overflow;
            }
            if (allowed->received_bytes_ > allowed->declared_length_ ||
                bytes_value > allowed->declared_length_ - allowed->received_bytes_) {
                return http2_remote_content_accounting_result::declared_length_exceeded;
            }
            allowed->received_bytes_ += bytes_value;
            return http2_remote_content_accounting_result::accepted;
        }
        return bytes_value == 0 ? http2_remote_content_accounting_result::accepted
                                : http2_remote_content_accounting_result::content_forbidden;
    }

    [[nodiscard]] constexpr const http2_remote_content_allowed_without_length* allowed_without_length()
        const& noexcept {
        return std::get_if<http2_remote_content_allowed_without_length>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_content_allowed_without_length* allowed_without_length()
        const&& = delete;

    [[nodiscard]] constexpr const http2_remote_content_allowed_known_length* allowed_known_length()
        const& noexcept {
        return std::get_if<http2_remote_content_allowed_known_length>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_content_allowed_known_length* allowed_known_length()
        const&& = delete;

    [[nodiscard]] constexpr const http2_remote_content_metadata_only_without_length*
    metadata_only_without_length() const& noexcept {
        return std::get_if<http2_remote_content_metadata_only_without_length>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_content_metadata_only_without_length*
    metadata_only_without_length() const&& = delete;

    [[nodiscard]] constexpr const http2_remote_content_metadata_only_known_length*
    metadata_only_known_length() const& noexcept {
        return std::get_if<http2_remote_content_metadata_only_known_length>(&state_);
    }
    [[nodiscard]] constexpr const http2_remote_content_metadata_only_known_length*
    metadata_only_known_length() const&& = delete;

    [[nodiscard]] bool terminal_length_valid() const noexcept {
        const auto* known = allowed_known_length();
        return known == nullptr || known->received_bytes() == known->declared_length();
    }

private:
    using state_type =
        std::variant<http2_remote_content_allowed_without_length, http2_remote_content_allowed_known_length,
            http2_remote_content_metadata_only_without_length, http2_remote_content_metadata_only_known_length>;

    state_type state_;
};

}  // namespace ruvia::detail
