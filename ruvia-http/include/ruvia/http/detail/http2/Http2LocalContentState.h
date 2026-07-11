#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ruvia::detail {

// Local HTTP message content accounting is distinct from the peer-body accounting
// stored on the same bidirectional stream. In particular, a server receives request
// content while independently producing response content.
enum class Http2LocalContentMode : std::uint8_t {
    kUnset,
    kForbidden,
    kUnbounded,
    kKnownLength
};

enum class Http2LocalContentCheck : std::uint8_t {
    kAccepted,
    kForbidden,
    kLengthExceeded,
    kLengthIncomplete
};

class Http2LocalContentState final {
public:
    void beginForbidden() noexcept {
        reset(Http2LocalContentMode::kForbidden, 0);
    }

    void beginUnbounded() noexcept {
        reset(Http2LocalContentMode::kUnbounded, 0);
    }

    void beginKnownLength(std::uint64_t length) noexcept {
        reset(Http2LocalContentMode::kKnownLength, length);
    }

    [[nodiscard]] Http2LocalContentMode mode() const noexcept {
        return mode_;
    }

    [[nodiscard]] bool hasKnownLength() const noexcept {
        return mode_ == Http2LocalContentMode::kKnownLength;
    }

    [[nodiscard]] std::uint64_t declaredLength() const noexcept {
        return declaredLength_;
    }

    [[nodiscard]] std::uint64_t acceptedBytes() const noexcept {
        return acceptedBytes_;
    }

    [[nodiscard]] std::uint64_t committedBytes() const noexcept {
        return committedBytes_;
    }

    // Transactional preflight for one submitData input. No counters change here.
    // A terminal known-length submission must complete the declared length exactly;
    // callers can retry a rejected input with a corrected terminal flag or size.
    [[nodiscard]] Http2LocalContentCheck checkAccept(
        std::size_t bytes,
        bool terminal) const noexcept {
        if (mode_ == Http2LocalContentMode::kForbidden) {
            return Http2LocalContentCheck::kForbidden;
        }

        const auto amount = static_cast<std::uint64_t>(bytes);
        if (amount > std::numeric_limits<std::uint64_t>::max() - acceptedBytes_) {
            return Http2LocalContentCheck::kLengthExceeded;
        }
        if (mode_ != Http2LocalContentMode::kKnownLength) {
            return Http2LocalContentCheck::kAccepted;
        }
        if (acceptedBytes_ > declaredLength_ ||
            amount > declaredLength_ - acceptedBytes_) {
            return Http2LocalContentCheck::kLengthExceeded;
        }
        if (terminal && acceptedBytes_ + amount != declaredLength_) {
            return Http2LocalContentCheck::kLengthIncomplete;
        }
        return Http2LocalContentCheck::kAccepted;
    }

    void accept(std::size_t bytes) noexcept {
        acceptedBytes_ += static_cast<std::uint64_t>(bytes);
    }

    // "Committed" means materialized as DATA payload in the connection's outbound
    // buffer, not flushed by a socket. The connection calls this at the single frame
    // emission point, including deferred WINDOW_UPDATE drains.
    void commit(std::size_t bytes) noexcept {
        committedBytes_ += static_cast<std::uint64_t>(bytes);
    }

    [[nodiscard]] bool lengthComplete() const noexcept {
        return mode_ != Http2LocalContentMode::kKnownLength ||
            acceptedBytes_ == declaredLength_;
    }

private:
    void reset(Http2LocalContentMode mode, std::uint64_t declaredLength) noexcept {
        mode_ = mode;
        declaredLength_ = declaredLength;
        acceptedBytes_ = 0;
        committedBytes_ = 0;
    }

    std::uint64_t declaredLength_{0};
    std::uint64_t acceptedBytes_{0};
    std::uint64_t committedBytes_{0};
    Http2LocalContentMode mode_{Http2LocalContentMode::kUnset};
};

}  // namespace ruvia::detail
