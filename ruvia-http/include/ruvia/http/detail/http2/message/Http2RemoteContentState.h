#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <variant>

namespace ruvia::detail {

class Http2RemoteContentState;

class Http2RemoteContentAllowedWithoutLength final {
public:
    [[nodiscard]] constexpr std::size_t receivedBytes() const noexcept {
        return receivedBytes_;
    }

private:
    friend class Http2RemoteContentState;

    explicit constexpr Http2RemoteContentAllowedWithoutLength(std::size_t receivedBytes = 0) noexcept
        : receivedBytes_(receivedBytes) {}

    std::size_t receivedBytes_{0};
};

class Http2RemoteContentAllowedKnownLength final {
public:
    [[nodiscard]] constexpr std::size_t declaredLength() const noexcept {
        return declaredLength_;
    }

    [[nodiscard]] constexpr std::size_t receivedBytes() const noexcept {
        return receivedBytes_;
    }

private:
    friend class Http2RemoteContentState;

    explicit constexpr Http2RemoteContentAllowedKnownLength(std::size_t declaredLength, std::size_t receivedBytes = 0) noexcept
        : declaredLength_(declaredLength),
          receivedBytes_(receivedBytes) {}

    std::size_t declaredLength_;
    std::size_t receivedBytes_{0};
};

class Http2RemoteContentMetadataOnlyWithoutLength final {
private:
    friend class Http2RemoteContentState;

    constexpr Http2RemoteContentMetadataOnlyWithoutLength() noexcept = default;
};

class Http2RemoteContentMetadataOnlyKnownLength final {
public:
    [[nodiscard]] constexpr std::size_t declaredLength() const noexcept {
        return declaredLength_;
    }

private:
    friend class Http2RemoteContentState;

    explicit constexpr Http2RemoteContentMetadataOnlyKnownLength(std::size_t declaredLength) noexcept
        : declaredLength_(declaredLength) {}

    std::size_t declaredLength_;
};

enum class Http2RemoteContentAccountingResult : std::uint8_t { kAccepted, kCounterOverflow, kDeclaredLengthExceeded, kContentForbidden };

// Content allowance, Content-Length ownership, and received-byte accounting are
// one exclusive state. HEAD/204/304 responses retain representation length
// metadata but cannot accidentally accept DATA as message content. account() is
// the only byte mutation: a rejected input leaves the active alternative intact.
class Http2RemoteContentState final {
public:
    constexpr Http2RemoteContentState() noexcept
        : state_(Http2RemoteContentAllowedWithoutLength()) {}

    [[nodiscard]] bool declareKnownLength(std::size_t length) noexcept {
        if (const auto* known = allowedKnownLength(); known != nullptr) {
            return known->declaredLength() == length;
        }
        if (const auto* known = metadataOnlyKnownLength(); known != nullptr) {
            return known->declaredLength() == length;
        }
        if (const auto* allowed = allowedWithoutLength(); allowed != nullptr) {
            if (allowed->receivedBytes() != 0) {
                return false;
            }
            state_ = State(Http2RemoteContentAllowedKnownLength(length));
            return true;
        }
        if (metadataOnlyWithoutLength() != nullptr) {
            state_ = State(Http2RemoteContentMetadataOnlyKnownLength(length));
            return true;
        }
        return false;
    }

    [[nodiscard]] bool selectMetadataOnly() noexcept {
        if (metadataOnlyWithoutLength() != nullptr || metadataOnlyKnownLength() != nullptr) {
            return true;
        }
        if (const auto* allowed = allowedWithoutLength(); allowed != nullptr) {
            if (allowed->receivedBytes() != 0) {
                return false;
            }
            state_ = State(Http2RemoteContentMetadataOnlyWithoutLength());
            return true;
        }
        if (const auto* allowed = allowedKnownLength(); allowed != nullptr) {
            if (allowed->receivedBytes() != 0) {
                return false;
            }
            state_ = State(Http2RemoteContentMetadataOnlyKnownLength(allowed->declaredLength()));
            return true;
        }
        return false;
    }

    [[nodiscard]] Http2RemoteContentAccountingResult account(std::size_t bytes) noexcept {
        if (auto* allowed = std::get_if<Http2RemoteContentAllowedWithoutLength>(&state_); allowed != nullptr) {
            if (bytes > std::numeric_limits<std::size_t>::max() - allowed->receivedBytes_) {
                return Http2RemoteContentAccountingResult::kCounterOverflow;
            }
            allowed->receivedBytes_ += bytes;
            return Http2RemoteContentAccountingResult::kAccepted;
        }
        if (auto* allowed = std::get_if<Http2RemoteContentAllowedKnownLength>(&state_); allowed != nullptr) {
            if (bytes > std::numeric_limits<std::size_t>::max() - allowed->receivedBytes_) {
                return Http2RemoteContentAccountingResult::kCounterOverflow;
            }
            if (allowed->receivedBytes_ > allowed->declaredLength_ || bytes > allowed->declaredLength_ - allowed->receivedBytes_) {
                return Http2RemoteContentAccountingResult::kDeclaredLengthExceeded;
            }
            allowed->receivedBytes_ += bytes;
            return Http2RemoteContentAccountingResult::kAccepted;
        }
        return bytes == 0 ? Http2RemoteContentAccountingResult::kAccepted : Http2RemoteContentAccountingResult::kContentForbidden;
    }

    [[nodiscard]] constexpr const Http2RemoteContentAllowedWithoutLength* allowedWithoutLength() const& noexcept {
        return std::get_if<Http2RemoteContentAllowedWithoutLength>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteContentAllowedWithoutLength* allowedWithoutLength() const&& = delete;

    [[nodiscard]] constexpr const Http2RemoteContentAllowedKnownLength* allowedKnownLength() const& noexcept {
        return std::get_if<Http2RemoteContentAllowedKnownLength>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteContentAllowedKnownLength* allowedKnownLength() const&& = delete;

    [[nodiscard]] constexpr const Http2RemoteContentMetadataOnlyWithoutLength* metadataOnlyWithoutLength() const& noexcept {
        return std::get_if<Http2RemoteContentMetadataOnlyWithoutLength>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteContentMetadataOnlyWithoutLength* metadataOnlyWithoutLength() const&& = delete;

    [[nodiscard]] constexpr const Http2RemoteContentMetadataOnlyKnownLength* metadataOnlyKnownLength() const& noexcept {
        return std::get_if<Http2RemoteContentMetadataOnlyKnownLength>(&state_);
    }
    [[nodiscard]] constexpr const Http2RemoteContentMetadataOnlyKnownLength* metadataOnlyKnownLength() const&& = delete;

    [[nodiscard]] bool terminalLengthValid() const noexcept {
        const auto* known = allowedKnownLength();
        return known == nullptr || known->receivedBytes() == known->declaredLength();
    }

private:
    using State = std::variant<Http2RemoteContentAllowedWithoutLength, Http2RemoteContentAllowedKnownLength, Http2RemoteContentMetadataOnlyWithoutLength, Http2RemoteContentMetadataOnlyKnownLength>;

    State state_;
};

}  // namespace ruvia::detail
