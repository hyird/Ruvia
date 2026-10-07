#pragma once

#include <cstdint>
#include <optional>
#include <variant>

namespace ruvia {

// One outbound content contract determines both Content-Length and END_STREAM.
// Public callers and the protocol engine use this same value directly.
class Http2RequestContent;

class Http2RequestWithoutContent final {
private:
    friend class Http2RequestContent;
    constexpr Http2RequestWithoutContent() noexcept = default;
};

class Http2KnownLengthRequestContent final {
public:
    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

private:
    friend class Http2RequestContent;
    explicit constexpr Http2KnownLengthRequestContent(std::uint64_t length) noexcept
        : length_(length) {}
    std::uint64_t length_;
};

class Http2StreamingRequestContent final {
public:
    [[nodiscard]] constexpr std::optional<std::uint64_t> expectedLength() const noexcept {
        return length_;
    }

private:
    friend class Http2RequestContent;
    explicit constexpr Http2StreamingRequestContent(std::optional<std::uint64_t> length) noexcept
        : length_(length) {}
    std::optional<std::uint64_t> length_{};
};

class Http2RequestContent final {
public:
    [[nodiscard]] static constexpr Http2RequestContent none() noexcept {
        return Http2RequestContent(Http2RequestWithoutContent());
    }
    [[nodiscard]] static constexpr Http2RequestContent knownLength(std::uint64_t length) noexcept {
        return Http2RequestContent(Http2KnownLengthRequestContent(length));
    }
    [[nodiscard]] static constexpr Http2RequestContent streaming(std::optional<std::uint64_t> length = {}) noexcept {
        return Http2RequestContent(Http2StreamingRequestContent(length));
    }
    [[nodiscard]] constexpr const Http2RequestWithoutContent* withoutContent() const& noexcept {
        return std::get_if<Http2RequestWithoutContent>(&value_);
    }
    const Http2RequestWithoutContent* withoutContent() const&& = delete;
    [[nodiscard]] constexpr const Http2KnownLengthRequestContent* knownLengthContent() const& noexcept {
        return std::get_if<Http2KnownLengthRequestContent>(&value_);
    }
    const Http2KnownLengthRequestContent* knownLengthContent() const&& = delete;
    [[nodiscard]] constexpr const Http2StreamingRequestContent* streamingContent() const& noexcept {
        return std::get_if<Http2StreamingRequestContent>(&value_);
    }
    const Http2StreamingRequestContent* streamingContent() const&& = delete;

private:
    using Value = std::variant<Http2RequestWithoutContent, Http2KnownLengthRequestContent,
        Http2StreamingRequestContent>;
    explicit constexpr Http2RequestContent(Http2RequestWithoutContent value) noexcept
        : value_(value) {}
    explicit constexpr Http2RequestContent(Http2KnownLengthRequestContent value) noexcept
        : value_(value) {}
    explicit constexpr Http2RequestContent(Http2StreamingRequestContent value) noexcept
        : value_(value) {}
    Value value_;
};

}  // namespace ruvia
