#pragma once

#include <cstdint>
#include <string_view>
#include <variant>

#include "ruvia/http/MultipartParser.h"

// Locating a multipart delimiter line in a byte run (RFC 2046 section 5.1.1):
// the CRLF--boundary sequence that separates parts, its closing form, and the
// "need more input" state a partial trailing match reports. Scanning only; part
// headers and the boundary parameter itself are parsed elsewhere.

namespace ruvia::detail {

class HttpMultipartDelimiterResult;

[[nodiscard]] inline HttpMultipartDelimiterResult httpMatchMultipartDelimiterLine(
    std::string_view input, const MultipartBoundary& boundary, bool inputFinished) noexcept;
[[nodiscard]] inline HttpMultipartDelimiterResult httpFindInitialMultipartDelimiter(
    std::string_view input, const MultipartBoundary& boundary, bool inputFinished) noexcept;
[[nodiscard]] inline HttpMultipartDelimiterResult httpFindMultipartBodyDelimiter(
    std::string_view input, const MultipartBoundary& boundary, bool inputFinished) noexcept;

class HttpMultipartDelimiterNoMatch final {
private:
    friend class HttpMultipartDelimiterResult;
    constexpr HttpMultipartDelimiterNoMatch() noexcept = default;
};

class HttpMultipartDelimiterNeedInput final {
public:
    // Initial search: offset of the leading "--". Body search: offset of the
    // CRLF that belongs to the delimiter rather than the preceding part.
    [[nodiscard]] constexpr std::size_t offset() const noexcept {
        return offset_;
    }

private:
    friend class HttpMultipartDelimiterResult;

    explicit constexpr HttpMultipartDelimiterNeedInput(std::size_t offset) noexcept
        : offset_(offset) {}

    std::size_t offset_;
};

class HttpMultipartPartDelimiter final {
public:
    [[nodiscard]] constexpr std::size_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::size_t lineBytes() const noexcept {
        return lineBytes_;
    }

private:
    friend class HttpMultipartDelimiterResult;

    constexpr HttpMultipartPartDelimiter(std::size_t offset, std::size_t lineBytes) noexcept
        : offset_(offset),
          lineBytes_(lineBytes) {}

    std::size_t offset_;
    std::size_t lineBytes_;
};

class HttpMultipartCloseDelimiter final {
public:
    [[nodiscard]] constexpr std::size_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::size_t lineBytes() const noexcept {
        return lineBytes_;
    }

private:
    friend class HttpMultipartDelimiterResult;

    constexpr HttpMultipartCloseDelimiter(std::size_t offset, std::size_t lineBytes) noexcept
        : offset_(offset),
          lineBytes_(lineBytes) {}

    std::size_t offset_;
    std::size_t lineBytes_;
};

// Delimiter scanning distinguishes absence, an input-boundary ambiguity, a
// regular part delimiter, and the terminal close delimiter. Only outcomes
// that found a candidate expose its offset; only complete delimiter lines
// expose their line length.
class HttpMultipartDelimiterResult final {
public:
    [[nodiscard]] constexpr const HttpMultipartDelimiterNoMatch* noMatch() const& noexcept {
        return std::get_if<HttpMultipartDelimiterNoMatch>(&value_);
    }
    const HttpMultipartDelimiterNoMatch* noMatch() const&& = delete;

    [[nodiscard]] constexpr const HttpMultipartDelimiterNeedInput* needInput() const& noexcept {
        return std::get_if<HttpMultipartDelimiterNeedInput>(&value_);
    }
    const HttpMultipartDelimiterNeedInput* needInput() const&& = delete;

    [[nodiscard]] constexpr const HttpMultipartPartDelimiter* part() const& noexcept {
        return std::get_if<HttpMultipartPartDelimiter>(&value_);
    }
    const HttpMultipartPartDelimiter* part() const&& = delete;

    [[nodiscard]] constexpr const HttpMultipartCloseDelimiter* close() const& noexcept {
        return std::get_if<HttpMultipartCloseDelimiter>(&value_);
    }
    const HttpMultipartCloseDelimiter* close() const&& = delete;

private:
    friend HttpMultipartDelimiterResult httpMatchMultipartDelimiterLine(
        std::string_view, const MultipartBoundary&, bool) noexcept;
    friend HttpMultipartDelimiterResult httpFindInitialMultipartDelimiter(
        std::string_view, const MultipartBoundary&, bool) noexcept;
    friend HttpMultipartDelimiterResult httpFindMultipartBodyDelimiter(
        std::string_view, const MultipartBoundary&, bool) noexcept;

    using Value = std::variant<HttpMultipartDelimiterNoMatch, HttpMultipartDelimiterNeedInput,
        HttpMultipartPartDelimiter, HttpMultipartCloseDelimiter>;

    template <typename Result>
    explicit constexpr HttpMultipartDelimiterResult(Result result) noexcept
        : value_(result) {}

    [[nodiscard]] static constexpr HttpMultipartDelimiterResult makeNoMatch() noexcept {
        return HttpMultipartDelimiterResult(HttpMultipartDelimiterNoMatch());
    }

    [[nodiscard]] static constexpr HttpMultipartDelimiterResult makeNeedInput(
        std::size_t offset = 0) noexcept {
        return HttpMultipartDelimiterResult(HttpMultipartDelimiterNeedInput(offset));
    }

    [[nodiscard]] static constexpr HttpMultipartDelimiterResult makePart(
        std::size_t offset, std::size_t lineBytes) noexcept {
        return HttpMultipartDelimiterResult(HttpMultipartPartDelimiter(offset, lineBytes));
    }

    [[nodiscard]] static constexpr HttpMultipartDelimiterResult makeClose(
        std::size_t offset, std::size_t lineBytes) noexcept {
        return HttpMultipartDelimiterResult(HttpMultipartCloseDelimiter(offset, lineBytes));
    }

    [[nodiscard]] constexpr HttpMultipartDelimiterResult rebased(std::size_t base) const noexcept {
        if (const auto* needInput = this->needInput()) {
            return makeNeedInput(base + needInput->offset());
        }
        if (const auto* part = this->part()) {
            return makePart(base + part->offset(), part->lineBytes());
        }
        if (const auto* close = this->close()) {
            return makeClose(base + close->offset(), close->lineBytes());
        }
        return makeNoMatch();
    }

    Value value_;
};

[[nodiscard]] inline bool httpMultipartMarkerPrefixMatches(
    std::string_view input, std::string_view boundary) noexcept {
    const auto expectedSize = boundary.size() + 2;
    const auto compared = std::min(input.size(), expectedSize);
    for (std::size_t index = 0; index < compared; ++index) {
        const char expected = index < 2 ? '-' : boundary[index - 2];
        if (input[index] != expected) {
            return false;
        }
    }
    return true;
}

// Matches one delimiter line beginning at its leading "--". RFC 2046
// transport-padding is accepted after a regular delimiter or after the closing
// "--". A closing delimiter ending exactly at the current buffer boundary is
// complete only when the I/O owner has signalled end-of-input.
[[nodiscard]] inline HttpMultipartDelimiterResult httpMatchMultipartDelimiterLine(
    std::string_view input, const MultipartBoundary& boundary, bool inputFinished) noexcept {
    const auto value = boundary.value();
    const auto markerSize = value.size() + 2;
    if (!httpMultipartMarkerPrefixMatches(input, value)) {
        return HttpMultipartDelimiterResult::makeNoMatch();
    }
    if (input.size() < markerSize) {
        return inputFinished ? HttpMultipartDelimiterResult::makeNoMatch()
                             : HttpMultipartDelimiterResult::makeNeedInput();
    }

    std::size_t cursor = markerSize;
    bool close = false;
    if (cursor < input.size() && input[cursor] == '-') {
        if (cursor + 1 >= input.size()) {
            return inputFinished ? HttpMultipartDelimiterResult::makeNoMatch()
                                 : HttpMultipartDelimiterResult::makeNeedInput();
        }
        if (input[cursor + 1] != '-') {
            return HttpMultipartDelimiterResult::makeNoMatch();
        }
        close = true;
        cursor += 2;
    }

    while (cursor < input.size() && (input[cursor] == ' ' || input[cursor] == '\t')) {
        ++cursor;
    }
    if (cursor == input.size()) {
        if (close && inputFinished) {
            return HttpMultipartDelimiterResult::makeClose(0, cursor);
        }
        return inputFinished ? HttpMultipartDelimiterResult::makeNoMatch()
                             : HttpMultipartDelimiterResult::makeNeedInput();
    }
    if (input[cursor] != '\r') {
        return HttpMultipartDelimiterResult::makeNoMatch();
    }
    if (cursor + 1 >= input.size()) {
        return inputFinished ? HttpMultipartDelimiterResult::makeNoMatch()
                             : HttpMultipartDelimiterResult::makeNeedInput();
    }
    if (input[cursor + 1] != '\n') {
        return HttpMultipartDelimiterResult::makeNoMatch();
    }
    return close ? HttpMultipartDelimiterResult::makeClose(0, cursor + 2)
                 : HttpMultipartDelimiterResult::makePart(0, cursor + 2);
}

[[nodiscard]] inline HttpMultipartDelimiterResult httpFindInitialMultipartDelimiter(
    std::string_view input, const MultipartBoundary& boundary, bool inputFinished) noexcept {
    if (!input.empty() && input.front() == '-') {
        auto match = httpMatchMultipartDelimiterLine(input, boundary, inputFinished);
        if (match.noMatch() == nullptr) {
            return match;
        }
    }

    for (auto prefix = input.find("\r\n--"); prefix != std::string_view::npos;
        prefix = input.find("\r\n--", prefix + 1)) {
        auto match =
            httpMatchMultipartDelimiterLine(input.substr(prefix + 2), boundary, inputFinished);
        if (match.noMatch() != nullptr) {
            continue;
        }
        return match.rebased(prefix + 2);
    }
    return HttpMultipartDelimiterResult::makeNoMatch();
}

[[nodiscard]] inline HttpMultipartDelimiterResult httpFindMultipartBodyDelimiter(
    std::string_view input, const MultipartBoundary& boundary, bool inputFinished) noexcept {
    for (auto prefix = input.find("\r\n--"); prefix != std::string_view::npos;
        prefix = input.find("\r\n--", prefix + 1)) {
        auto match =
            httpMatchMultipartDelimiterLine(input.substr(prefix + 2), boundary, inputFinished);
        if (match.noMatch() != nullptr) {
            continue;
        }
        return match.rebased(prefix);
    }
    return HttpMultipartDelimiterResult::makeNoMatch();
}

}  // namespace ruvia::detail
