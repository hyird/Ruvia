#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <variant>

namespace ruvia::detail {

// Local HTTP message content accounting is distinct from the peer-body accounting
// stored on the same bidirectional stream. In particular, a server receives request
// content while independently producing response content.
class http2_local_content_state;

class http2_local_content_unset final {
private:
    friend class http2_local_content_state;

    constexpr http2_local_content_unset() noexcept = default;
};

class http2_local_content_forbidden final {
private:
    friend class http2_local_content_state;

    constexpr http2_local_content_forbidden() noexcept = default;
};

class http2_local_content_unbounded final {
private:
    friend class http2_local_content_state;

    constexpr http2_local_content_unbounded() noexcept = default;
};

class http2_local_content_known_length final {
public:
    [[nodiscard]] constexpr std::uint64_t declared_length() const noexcept {
        return declared_length_;
    }

private:
    friend class http2_local_content_state;

    explicit constexpr http2_local_content_known_length(std::uint64_t declared_length) noexcept
        : declared_length_(declared_length) {}

    std::uint64_t declared_length_;
};

enum class http2_local_content_check : std::uint8_t {
    accepted,
    not_started,
    forbidden,
    length_exceeded,
    length_incomplete
};

class http2_local_content_state final {
public:
    constexpr http2_local_content_state() noexcept
        : content_(http2_local_content_unset()) {}

    void begin_forbidden() noexcept {
        reset(content_type(http2_local_content_forbidden()));
    }

    void begin_unbounded() noexcept {
        reset(content_type(http2_local_content_unbounded()));
    }

    void begin_known_length(std::uint64_t length) noexcept {
        reset(content_type(http2_local_content_known_length(length)));
    }

    [[nodiscard]] constexpr const http2_local_content_unset* unset() const& noexcept {
        return std::get_if<http2_local_content_unset>(&content_);
    }
    [[nodiscard]] constexpr const http2_local_content_unset* unset() const&& = delete;

    [[nodiscard]] constexpr const http2_local_content_forbidden* forbidden() const& noexcept {
        return std::get_if<http2_local_content_forbidden>(&content_);
    }
    [[nodiscard]] constexpr const http2_local_content_forbidden* forbidden() const&& = delete;

    [[nodiscard]] constexpr const http2_local_content_unbounded* unbounded() const& noexcept {
        return std::get_if<http2_local_content_unbounded>(&content_);
    }
    [[nodiscard]] constexpr const http2_local_content_unbounded* unbounded() const&& = delete;

    [[nodiscard]] constexpr const http2_local_content_known_length* known_length() const& noexcept {
        return std::get_if<http2_local_content_known_length>(&content_);
    }
    [[nodiscard]] constexpr const http2_local_content_known_length* known_length() const&& = delete;

    [[nodiscard]] std::uint64_t accepted_bytes() const noexcept {
        return accepted_bytes_;
    }

    [[nodiscard]] std::uint64_t committed_bytes() const noexcept {
        return committed_bytes_;
    }

    // Transactional preflight for one submit_data input. No counters change here.
    // A terminal known-length submission must complete the declared length exactly;
    // callers can retry a rejected input with a corrected terminal flag or size.
    [[nodiscard]] http2_local_content_check check_accept(
        std::size_t bytes_value, bool terminal) const noexcept {
        if (unset() != nullptr) {
            return http2_local_content_check::not_started;
        }
        if (forbidden() != nullptr) {
            return http2_local_content_check::forbidden;
        }

        const auto amount = static_cast<std::uint64_t>(bytes_value);
        if (amount > std::numeric_limits<std::uint64_t>::max() - accepted_bytes_) {
            return http2_local_content_check::length_exceeded;
        }
        const auto* known_length_content = known_length();
        if (known_length_content == nullptr) {
            return http2_local_content_check::accepted;
        }
        const auto declared_length = known_length_content->declared_length();
        if (accepted_bytes_ > declared_length || amount > declared_length - accepted_bytes_) {
            return http2_local_content_check::length_exceeded;
        }
        if (terminal && accepted_bytes_ + amount != declared_length) {
            return http2_local_content_check::length_incomplete;
        }
        return http2_local_content_check::accepted;
    }

    void accept(std::size_t bytes_value) noexcept {
        accepted_bytes_ += static_cast<std::uint64_t>(bytes_value);
    }

    // "Committed" means materialized as DATA payload in the connection's outbound
    // buffer, not flushed by a socket. The connection calls this at the single frame
    // emission point, including deferred WINDOW_UPDATE drains.
    void commit(std::size_t bytes_value) noexcept {
        committed_bytes_ += static_cast<std::uint64_t>(bytes_value);
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
    using content_type = std::variant<http2_local_content_unset, http2_local_content_forbidden,
        http2_local_content_unbounded, http2_local_content_known_length>;

    void reset(content_type content) noexcept {
        content_ = content;
        accepted_bytes_ = 0;
        committed_bytes_ = 0;
    }

    content_type content_;
    std::uint64_t accepted_bytes_{0};
    std::uint64_t committed_bytes_{0};
};

}  // namespace ruvia::detail
