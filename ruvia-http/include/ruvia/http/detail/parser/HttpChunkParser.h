#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia::detail {

enum class HttpChunkScanError : std::uint8_t { kInvalidSize, kSizeOverflow, kInvalidExtension, kInvalidCrlf, kInvalidTrailer, kTooLarge };

class HttpChunkTrailerField final {
public:
    [[nodiscard]] constexpr std::string_view name() const& noexcept { return name_; }
    std::string_view name() const&& = delete;
    [[nodiscard]] constexpr std::string_view value() const& noexcept { return value_; }
    std::string_view value() const&& = delete;

private:
    friend class HttpChunkTrailerParseResult;
    friend class HttpChunkTrailerParser;

    constexpr HttpChunkTrailerField(std::string_view name, std::string_view value) noexcept
        : name_(name), value_(value) {}

    std::string_view name_;
    std::string_view value_;
};

class HttpChunkTrailerEnd final {
private:
    friend class HttpChunkTrailerParseResult;
    friend class HttpChunkTrailerParser;
    constexpr HttpChunkTrailerEnd() noexcept = default;
};

class HttpChunkTrailerFailure final {
public:
    [[nodiscard]] constexpr HttpChunkScanError error() const noexcept { return error_; }

private:
    friend class HttpChunkTrailerParseResult;
    friend class HttpChunkTrailerParser;

    explicit constexpr HttpChunkTrailerFailure(HttpChunkScanError error) noexcept
        : error_(error) {}

    HttpChunkScanError error_;
};

class HttpChunkTrailerParseResult final {
public:
    [[nodiscard]] const HttpChunkTrailerField* field() const& noexcept {
        return std::get_if<HttpChunkTrailerField>(&value_);
    }
    const HttpChunkTrailerField* field() const&& = delete;
    [[nodiscard]] const HttpChunkTrailerEnd* end() const& noexcept {
        return std::get_if<HttpChunkTrailerEnd>(&value_);
    }
    const HttpChunkTrailerEnd* end() const&& = delete;
    [[nodiscard]] const HttpChunkTrailerFailure* failure() const& noexcept {
        return std::get_if<HttpChunkTrailerFailure>(&value_);
    }
    const HttpChunkTrailerFailure* failure() const&& = delete;

private:
    friend class HttpChunkTrailerParser;
    using Value = std::variant<HttpChunkTrailerField, HttpChunkTrailerEnd, HttpChunkTrailerFailure>;

    template <typename Result>
    explicit constexpr HttpChunkTrailerParseResult(Result result) noexcept
        : value_(result) {}

    Value value_;
};

// Iterates a validated-or-untrusted trailer block without allocation. Field
// views borrow the block passed to the constructor and remain valid until that
// storage is mutated.
class HttpChunkTrailerParser final {
public:
    explicit constexpr HttpChunkTrailerParser(std::string_view trailers) noexcept
        : trailers_(trailers) {}

    template <HttpTemporaryOwningCharString Trailers>
    explicit HttpChunkTrailerParser(Trailers&&) = delete;

    [[nodiscard]] HttpChunkTrailerParseResult next() noexcept;

private:
    [[nodiscard]] HttpChunkTrailerParseResult fail(HttpChunkScanError error) noexcept;

    std::string_view trailers_;
    std::size_t cursor_{0};
    std::size_t fieldCount_{0};
    std::optional<HttpChunkScanError> failure_;
};

class HttpChunkScanNeedMore final {
private:
    friend class HttpChunkScanResult;
    constexpr HttpChunkScanNeedMore() noexcept = default;
};

class HttpChunkScanComplete final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend class HttpChunkScanResult;

    explicit constexpr HttpChunkScanComplete(std::size_t consumedBytes) noexcept
        : consumedBytes_(consumedBytes) {}

    std::size_t consumedBytes_;
};

class HttpChunkScanFailure final {
public:
    [[nodiscard]] constexpr HttpChunkScanError error() const noexcept {
        return error_;
    }

private:
    friend class HttpChunkScanResult;

    explicit constexpr HttpChunkScanFailure(HttpChunkScanError error) noexcept
        : error_(error) {}

    HttpChunkScanError error_;
};

// Whole-message chunk framing has three mutually exclusive outcomes. Only a
// complete result owns a consumed byte count; need-more and failure cannot
// accidentally expose a plausible framing boundary.
class HttpChunkScanResult final {
public:
    [[nodiscard]] const HttpChunkScanNeedMore* needMore() const& noexcept {
        return std::get_if<HttpChunkScanNeedMore>(&value_);
    }
    const HttpChunkScanNeedMore* needMore() const&& = delete;

    [[nodiscard]] const HttpChunkScanComplete* complete() const& noexcept {
        return std::get_if<HttpChunkScanComplete>(&value_);
    }
    const HttpChunkScanComplete* complete() const&& = delete;

    [[nodiscard]] const HttpChunkScanFailure* failure() const& noexcept {
        return std::get_if<HttpChunkScanFailure>(&value_);
    }
    const HttpChunkScanFailure* failure() const&& = delete;

private:
    friend HttpChunkScanResult scanHttpChunkedBody(std::string_view body) noexcept;

    using Value = std::variant<HttpChunkScanNeedMore, HttpChunkScanComplete, HttpChunkScanFailure>;

    template <typename Result>
    explicit HttpChunkScanResult(Result result) noexcept
        : value_(std::move(result)) {}

    [[nodiscard]] static HttpChunkScanResult makeNeedMore() noexcept {
        return HttpChunkScanResult(HttpChunkScanNeedMore());
    }

    [[nodiscard]] static HttpChunkScanResult makeComplete(std::size_t consumedBytes) noexcept {
        return HttpChunkScanResult(HttpChunkScanComplete(consumedBytes));
    }

    [[nodiscard]] static HttpChunkScanResult makeFailure(HttpChunkScanError error) noexcept {
        return HttpChunkScanResult(HttpChunkScanFailure(error));
    }

    Value value_;
};

[[nodiscard]] bool parseHttpChunkSize(std::string_view value, std::size_t& size) noexcept;
[[nodiscard]] std::optional<HttpChunkScanError> validateHttpChunkTrailers(std::string_view trailers) noexcept;
[[nodiscard]] HttpChunkScanResult scanHttpChunkedBody(std::string_view body) noexcept;

}  // namespace ruvia::detail
