#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <variant>

namespace ruvia::detail {

// Local HTTP message content accounting is distinct from the peer-body accounting
// stored on the same bidirectional stream. In particular, a server receives request
// content while independently producing response content.
class http_local_content_known_length final {
public:
    [[nodiscard]] constexpr std::uint64_t declared_length() const noexcept {
        return declared_length_;
    }

private:
    friend class http_local_content_state;

    explicit constexpr http_local_content_known_length(std::uint64_t declared_length) noexcept
        : declared_length_(declared_length) {}

    std::uint64_t declared_length_;
};

enum class http_local_content_check : std::uint8_t {
    accepted,
    not_started,
    forbidden,
    length_exceeded,
    length_incomplete
};

class http_local_content_state final {
    enum class phase_type : std::uint8_t { unset,
        forbidden,
        unbounded };

public:
    constexpr http_local_content_state() noexcept = default;

    void begin_forbidden() noexcept {
        reset(content_type(phase_type::forbidden));
    }

    void begin_unbounded() noexcept {
        reset(content_type(phase_type::unbounded));
    }

    void begin_known_length(std::uint64_t length) noexcept {
        reset(content_type(http_local_content_known_length(length)));
    }

    [[nodiscard]] constexpr const void* unset() const& noexcept {
        return has_phase(phase_type::unset) ? this : nullptr;
    }
    const void* unset() const&& = delete;

    [[nodiscard]] constexpr const void* forbidden() const& noexcept {
        return has_phase(phase_type::forbidden) ? this : nullptr;
    }
    const void* forbidden() const&& = delete;

    [[nodiscard]] constexpr const void* unbounded() const& noexcept {
        return has_phase(phase_type::unbounded) ? this : nullptr;
    }
    const void* unbounded() const&& = delete;

    [[nodiscard]] constexpr const http_local_content_known_length* known_length() const& noexcept {
        return std::get_if<http_local_content_known_length>(&content_);
    }
    const http_local_content_known_length* known_length() const&& = delete;

    // Transactional preflight for one submit_data input. No counters change here.
    // A terminal known-length submission must complete the declared length exactly;
    // callers can retry a rejected input with a corrected terminal flag or size.
    [[nodiscard]] http_local_content_check check_accept(
        std::size_t bytes_value, bool terminal) const noexcept {
        if (unset() != nullptr) {
            return http_local_content_check::not_started;
        }
        if (forbidden() != nullptr) {
            return http_local_content_check::forbidden;
        }

        const auto amount = static_cast<std::uint64_t>(bytes_value);
        if (amount > std::numeric_limits<std::uint64_t>::max() - accepted_bytes_) {
            return http_local_content_check::length_exceeded;
        }
        const auto* known_length_content = known_length();
        if (known_length_content == nullptr) {
            return http_local_content_check::accepted;
        }
        const auto declared_length = known_length_content->declared_length();
        if (accepted_bytes_ > declared_length || amount > declared_length - accepted_bytes_) {
            return http_local_content_check::length_exceeded;
        }
        if (terminal && accepted_bytes_ + amount != declared_length) {
            return http_local_content_check::length_incomplete;
        }
        return http_local_content_check::accepted;
    }

    void commit(std::size_t bytes_value) noexcept {
        accepted_bytes_ += static_cast<std::uint64_t>(bytes_value);
    }

    [[nodiscard]] bool length_complete() const noexcept {
        if (unset() != nullptr) {
            return false;
        }
        const auto* known_length_content = known_length();
        return known_length_content == nullptr ||
               accepted_bytes_ == known_length_content->declared_length();
    }

private:
    using content_type = std::variant<phase_type, http_local_content_known_length>;

    [[nodiscard]] constexpr bool has_phase(phase_type phase) const noexcept {
        const auto* current = std::get_if<phase_type>(&content_);
        return current != nullptr && *current == phase;
    }

    void reset(content_type content) noexcept {
        content_ = content;
        accepted_bytes_ = 0;
    }

    content_type content_{phase_type::unset};
    std::uint64_t accepted_bytes_{0};
};

}  // namespace ruvia::detail
