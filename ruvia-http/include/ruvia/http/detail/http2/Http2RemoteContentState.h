#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <variant>

namespace ruvia::detail {

class Http2RemoteContentState;

class Http2RemoteContentWithoutLength final {
private:
    friend class Http2RemoteContentState;

    constexpr Http2RemoteContentWithoutLength() noexcept = default;
};

class Http2RemoteContentKnownLength final {
public:
    [[nodiscard]] constexpr std::size_t declaredLength() const noexcept {
        return declaredLength_;
    }

private:
    friend class Http2RemoteContentState;

    explicit constexpr Http2RemoteContentKnownLength(
        std::size_t declaredLength) noexcept
        : declaredLength_(declaredLength) {}

    std::size_t declaredLength_;
};

enum class Http2RemoteContentCheck : std::uint8_t {
    kAccepted,
    kCounterOverflow,
    kDeclaredLengthExceeded
};

// Stream-owned accounting for the content received from the peer. A missing
// Content-Length and an explicitly declared zero are different alternatives;
// only the known-length alternative owns a declared length. DATA acceptance is
// transactional: checkAccept() does not mutate the byte count, and accept() is
// called only after every protocol and product limit has accepted that input.
class Http2RemoteContentState final {
public:
    constexpr Http2RemoteContentState() noexcept
        : content_(Http2RemoteContentWithoutLength()) {}

    [[nodiscard]] bool declareKnownLength(std::size_t length) noexcept {
        if (const auto* known = knownLength(); known != nullptr) {
            return known->declaredLength() == length;
        }
        if (receivedBytes_ != 0) {
            return false;
        }
        content_ = Content(Http2RemoteContentKnownLength(length));
        return true;
    }

    [[nodiscard]] constexpr const Http2RemoteContentWithoutLength*
    withoutLength() const noexcept {
        return std::get_if<Http2RemoteContentWithoutLength>(&content_);
    }

    [[nodiscard]] constexpr const Http2RemoteContentKnownLength*
    knownLength() const noexcept {
        return std::get_if<Http2RemoteContentKnownLength>(&content_);
    }

    [[nodiscard]] std::size_t receivedBytes() const noexcept {
        return receivedBytes_;
    }

    [[nodiscard]] Http2RemoteContentCheck checkAccept(
        std::size_t bytes) const noexcept {
        if (bytes > std::numeric_limits<std::size_t>::max() - receivedBytes_) {
            return Http2RemoteContentCheck::kCounterOverflow;
        }
        const auto* known = knownLength();
        if (known != nullptr &&
            (receivedBytes_ > known->declaredLength() ||
             bytes > known->declaredLength() - receivedBytes_)) {
            return Http2RemoteContentCheck::kDeclaredLengthExceeded;
        }
        return Http2RemoteContentCheck::kAccepted;
    }

    void accept(std::size_t bytes) noexcept {
        receivedBytes_ += bytes;
    }

    // Consulted only when END_STREAM is observed. Without a Content-Length,
    // END_STREAM itself delimits the content; with one, the DATA total must match.
    [[nodiscard]] bool terminalLengthValid() const noexcept {
        const auto* known = knownLength();
        return known == nullptr || receivedBytes_ == known->declaredLength();
    }

private:
    using Content = std::variant<
        Http2RemoteContentWithoutLength,
        Http2RemoteContentKnownLength>;

    Content content_;
    std::size_t receivedBytes_{0};
};

}  // namespace ruvia::detail
