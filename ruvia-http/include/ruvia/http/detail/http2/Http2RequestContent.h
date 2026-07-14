#pragma once

#include <cstdint>
#include <variant>

namespace ruvia::detail {

// The outbound request-content contract is chosen before the initial HEADERS are
// serialized. Keeping it as one value prevents Content-Length and END_STREAM from
// becoming independent sources of truth.
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

    explicit constexpr Http2KnownLengthRequestContent(
        std::uint64_t length) noexcept
        : length_(length) {}

    std::uint64_t length_;
};

class Http2StreamingRequestContent final {
private:
    friend class Http2RequestContent;

    constexpr Http2StreamingRequestContent() noexcept = default;
};

class Http2RequestContent final {
public:
    [[nodiscard]] static constexpr Http2RequestContent none() noexcept {
        return Http2RequestContent(Http2RequestWithoutContent());
    }

    [[nodiscard]] static constexpr Http2RequestContent knownLength(
        std::uint64_t length) noexcept {
        return Http2RequestContent(Http2KnownLengthRequestContent(length));
    }

    [[nodiscard]] static constexpr Http2RequestContent streaming() noexcept {
        return Http2RequestContent(Http2StreamingRequestContent());
    }

    [[nodiscard]] constexpr const Http2RequestWithoutContent*
    withoutContent() const & noexcept {
        return std::get_if<Http2RequestWithoutContent>(&content_);
    }
    [[nodiscard]] constexpr const Http2RequestWithoutContent*
    withoutContent() const && = delete;

    [[nodiscard]] constexpr const Http2KnownLengthRequestContent*
    knownLengthContent() const & noexcept {
        return std::get_if<Http2KnownLengthRequestContent>(&content_);
    }
    [[nodiscard]] constexpr const Http2KnownLengthRequestContent*
    knownLengthContent() const && = delete;

    [[nodiscard]] constexpr const Http2StreamingRequestContent*
    streamingContent() const & noexcept {
        return std::get_if<Http2StreamingRequestContent>(&content_);
    }
    [[nodiscard]] constexpr const Http2StreamingRequestContent*
    streamingContent() const && = delete;

private:
    using Content = std::variant<
        Http2RequestWithoutContent,
        Http2KnownLengthRequestContent,
        Http2StreamingRequestContent>;

    explicit constexpr Http2RequestContent(
        Http2RequestWithoutContent content) noexcept
        : content_(content) {}

    explicit constexpr Http2RequestContent(
        Http2KnownLengthRequestContent content) noexcept
        : content_(content) {}

    explicit constexpr Http2RequestContent(
        Http2StreamingRequestContent content) noexcept
        : content_(content) {}

    Content content_;
};

}  // namespace ruvia::detail
