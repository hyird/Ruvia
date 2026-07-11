#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <variant>

namespace ruvia::detail {

// Local HTTP message content accounting is distinct from the peer-body accounting
// stored on the same bidirectional stream. In particular, a server receives request
// content while independently producing response content.
class Http2LocalContentState;

class Http2LocalContentUnset final {
private:
    friend class Http2LocalContentState;

    constexpr Http2LocalContentUnset() noexcept = default;
};

class Http2LocalContentForbidden final {
private:
    friend class Http2LocalContentState;

    constexpr Http2LocalContentForbidden() noexcept = default;
};

class Http2LocalContentUnbounded final {
private:
    friend class Http2LocalContentState;

    constexpr Http2LocalContentUnbounded() noexcept = default;
};

class Http2LocalContentKnownLength final {
public:
    [[nodiscard]] constexpr std::uint64_t declaredLength() const noexcept {
        return declaredLength_;
    }

private:
    friend class Http2LocalContentState;

    explicit constexpr Http2LocalContentKnownLength(
        std::uint64_t declaredLength) noexcept
        : declaredLength_(declaredLength) {}

    std::uint64_t declaredLength_;
};

enum class Http2LocalContentCheck : std::uint8_t {
    kAccepted,
    kNotStarted,
    kForbidden,
    kLengthExceeded,
    kLengthIncomplete
};

class Http2LocalContentState final {
public:
    constexpr Http2LocalContentState() noexcept
        : content_(Http2LocalContentUnset()) {}

    void beginForbidden() noexcept {
        reset(Content(Http2LocalContentForbidden()));
    }

    void beginUnbounded() noexcept {
        reset(Content(Http2LocalContentUnbounded()));
    }

    void beginKnownLength(std::uint64_t length) noexcept {
        reset(Content(Http2LocalContentKnownLength(length)));
    }

    [[nodiscard]] constexpr const Http2LocalContentUnset*
    unset() const noexcept {
        return std::get_if<Http2LocalContentUnset>(&content_);
    }

    [[nodiscard]] constexpr const Http2LocalContentForbidden*
    forbidden() const noexcept {
        return std::get_if<Http2LocalContentForbidden>(&content_);
    }

    [[nodiscard]] constexpr const Http2LocalContentUnbounded*
    unbounded() const noexcept {
        return std::get_if<Http2LocalContentUnbounded>(&content_);
    }

    [[nodiscard]] constexpr const Http2LocalContentKnownLength*
    knownLength() const noexcept {
        return std::get_if<Http2LocalContentKnownLength>(&content_);
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
        if (unset() != nullptr) {
            return Http2LocalContentCheck::kNotStarted;
        }
        if (forbidden() != nullptr) {
            return Http2LocalContentCheck::kForbidden;
        }

        const auto amount = static_cast<std::uint64_t>(bytes);
        if (amount > std::numeric_limits<std::uint64_t>::max() - acceptedBytes_) {
            return Http2LocalContentCheck::kLengthExceeded;
        }
        const auto* knownLengthContent = knownLength();
        if (knownLengthContent == nullptr) {
            return Http2LocalContentCheck::kAccepted;
        }
        const auto declaredLength = knownLengthContent->declaredLength();
        if (acceptedBytes_ > declaredLength ||
            amount > declaredLength - acceptedBytes_) {
            return Http2LocalContentCheck::kLengthExceeded;
        }
        if (terminal && acceptedBytes_ + amount != declaredLength) {
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
        if (unset() != nullptr) {
            return false;
        }
        const auto* knownLengthContent = knownLength();
        return knownLengthContent == nullptr ||
            acceptedBytes_ == knownLengthContent->declaredLength();
    }

private:
    using Content = std::variant<
        Http2LocalContentUnset,
        Http2LocalContentForbidden,
        Http2LocalContentUnbounded,
        Http2LocalContentKnownLength>;

    void reset(Content content) noexcept {
        content_ = content;
        acceptedBytes_ = 0;
        committedBytes_ = 0;
    }

    Content content_;
    std::uint64_t acceptedBytes_{0};
    std::uint64_t committedBytes_{0};
};

}  // namespace ruvia::detail
