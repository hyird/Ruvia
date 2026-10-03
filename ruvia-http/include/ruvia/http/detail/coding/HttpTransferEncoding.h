#pragma once

#include <algorithm>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/detail/coding/HttpTransferCoding.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia::detail {

[[nodiscard]] inline bool httpValidTransferParameterValue(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() != '"') {
        return std::ranges::all_of(value,
            [](char byte) noexcept { return isHttpTokenChar(static_cast<unsigned char>(byte)); });
    }
    if (value.size() < 2 || value.back() != '"') {
        return false;
    }
    const auto end = value.size() - 1;
    for (std::size_t cursor = 1; cursor < end; ++cursor) {
        auto byte = static_cast<unsigned char>(value[cursor]);
        if (byte == '\\') {
            if (++cursor == end) {
                return false;
            }
            byte = static_cast<unsigned char>(value[cursor]);
            if (byte != '\t' && byte != ' ' && (byte < 0x21 || byte > 0x7e) && byte < 0x80) {
                return false;
            }
        } else if (byte == '"' ||
                   (byte != '\t' && byte != ' ' && byte != 0x21 && (byte < 0x23 || byte > 0x5b) &&
                       (byte < 0x5d || byte > 0x7e) && byte < 0x80)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool httpParseTransferCodingSyntax(
    std::string_view item, std::string_view& coding, bool& hasParameters) noexcept {
    const auto firstSemicolon = httpFindUnquotedDelimiter(item, 0, ';');
    coding = httpTrimOws(item.substr(0, firstSemicolon));
    if (coding.empty()) {
        return false;
    }
    for (const auto byte : coding) {
        if (!isHttpTokenChar(static_cast<unsigned char>(byte))) {
            return false;
        }
    }

    hasParameters = firstSemicolon < item.size();
    auto start = firstSemicolon;
    while (start < item.size()) {
        ++start;
        const auto end = httpFindUnquotedDelimiter(item, start, ';');
        const auto parameter = httpTrimOws(item.substr(start, end - start));
        const auto equals = parameter.find('=');
        if (equals == std::string_view::npos) {
            return false;
        }
        const auto name = httpTrimOws(parameter.substr(0, equals));
        const auto value = httpTrimOws(parameter.substr(equals + 1));
        if (name.empty()) {
            return false;
        }
        for (const auto byte : name) {
            if (!isHttpTokenChar(static_cast<unsigned char>(byte))) {
                return false;
            }
        }
        if (!httpValidTransferParameterValue(value)) {
            return false;
        }
        start = end;
    }
    return true;
}

template <HttpTemporaryOwningCharString Item>
bool httpParseTransferCodingSyntax(Item&&, std::string_view&, bool&) = delete;

enum class HttpTransferEncodingParseStatus : std::uint8_t { kOk,
    kMalformed,
    kUnsupported };

class HttpNonChunkedTransferEncoding final {
public:
    [[nodiscard]] const HttpTransferCodings& transferCodings() const noexcept {
        return transferCodings_;
    }

private:
    friend class HttpTransferEncodingValue;

    explicit HttpNonChunkedTransferEncoding(HttpTransferCodings transferCodings)
        : transferCodings_(std::move(transferCodings)) {}

    HttpTransferCodings transferCodings_;
};

class HttpFinalChunkedTransferEncoding final {
public:
    [[nodiscard]] const HttpTransferCodings& transferCodings() const noexcept {
        return transferCodings_;
    }

private:
    friend class HttpTransferEncodingValue;

    explicit HttpFinalChunkedTransferEncoding(HttpTransferCodings transferCodings)
        : transferCodings_(std::move(transferCodings)) {}

    HttpTransferCodings transferCodings_;
};

class HttpTransferEncodingValue final {
public:
    [[nodiscard]] const HttpNonChunkedTransferEncoding* nonChunked() const& noexcept {
        return std::get_if<HttpNonChunkedTransferEncoding>(&value_);
    }
    const HttpNonChunkedTransferEncoding* nonChunked() const&& = delete;

    [[nodiscard]] const HttpFinalChunkedTransferEncoding* finalChunked() const& noexcept {
        return std::get_if<HttpFinalChunkedTransferEncoding>(&value_);
    }
    const HttpFinalChunkedTransferEncoding* finalChunked() const&& = delete;

private:
    friend class HttpTransferEncodingState;

    using Value = std::variant<HttpNonChunkedTransferEncoding, HttpFinalChunkedTransferEncoding>;

    [[nodiscard]] static HttpTransferEncodingValue makeNonChunked(
        HttpTransferCodings transferCodings) {
        return HttpTransferEncodingValue(HttpNonChunkedTransferEncoding(std::move(transferCodings)));
    }

    [[nodiscard]] static HttpTransferEncodingValue makeFinalChunked(
        HttpTransferCodings transferCodings) {
        return HttpTransferEncodingValue(HttpFinalChunkedTransferEncoding(std::move(transferCodings)));
    }

    explicit HttpTransferEncodingValue(HttpNonChunkedTransferEncoding value)
        : value_(std::move(value)) {}

    explicit HttpTransferEncodingValue(HttpFinalChunkedTransferEncoding value)
        : value_(std::move(value)) {}

    Value value_;
};

static_assert(std::is_nothrow_move_constructible_v<HttpTransferEncodingValue>);

// Field lines form one ordered list. Unknown but syntactically valid codings
// are remembered while later items/fields are still checked for malformed syntax.
class HttpTransferEncodingState final {
public:
    explicit HttpTransferEncodingState(
        std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : resource_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

    [[nodiscard]] HttpTransferEncodingParseStatus parseField(std::string_view fieldValue) {
        // The published value is the sole owner of the committed coding list.
        HttpTransferCodings next_codings(resource_);
        bool finalChunked = false;
        if (value_) {
            if (const auto* chunked = value_->finalChunked()) {
                next_codings = chunked->transferCodings();
                finalChunked = true;
            } else {
                next_codings = value_->nonChunked()->transferCodings();
            }
        }
        bool unsupported = unsupported_;
        bool sawItem = false;
        bool malformed = false;
        httpVisitCommaSeparatedQuotedItems(fieldValue,
            [&next_codings, &finalChunked, &unsupported, &sawItem, &malformed](
                std::string_view item) {
                sawItem = true;
                std::string_view coding;
                bool hasParameters = false;
                if (finalChunked || !httpParseTransferCodingSyntax(item, coding, hasParameters)) {
                    malformed = true;
                    return false;
                }
                if (httpAsciiEqualsIgnoreCase(coding, "chunked")) {
                    if (hasParameters) {
                        malformed = true;
                        return false;
                    }
                    finalChunked = true;
                    return true;
                }
                const bool gzip = httpAsciiEqualsIgnoreCase(coding, "gzip") ||
                                  httpAsciiEqualsIgnoreCase(coding, "x-gzip");
                const bool deflate = httpAsciiEqualsIgnoreCase(coding, "deflate");
                if (gzip || deflate) {
                    if (hasParameters) {
                        malformed = true;
                        return false;
                    }
                    if (next_codings.values.size() == kMaxTransferCodings) {
                        unsupported = true;
                        return true;
                    }
                    next_codings.values.push_back(gzip ? HttpTransferCoding::kGzip
                                                       : HttpTransferCoding::kDeflate);
                    return true;
                }
                unsupported = true;
                return true;
            });
        if (malformed || !sawItem) {
            return HttpTransferEncodingParseStatus::kMalformed;
        }
        auto next_value = finalChunked ? HttpTransferEncodingValue::makeFinalChunked(std::move(next_codings))
                                       : HttpTransferEncodingValue::makeNonChunked(std::move(next_codings));
        // All allocations precede publication. Sequence move construction only
        // transfers its PMR buffer, including with MSVC iterator debugging.
        value_.emplace(std::move(next_value));
        unsupported_ = unsupported;
        return unsupported_ ? HttpTransferEncodingParseStatus::kUnsupported
                            : HttpTransferEncodingParseStatus::kOk;
    }

    [[nodiscard]] const std::optional<HttpTransferEncodingValue>& value() const noexcept {
        return value_;
    }
    [[nodiscard]] bool unsupported() const noexcept {
        return unsupported_;
    }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return resource_;
    }

private:
    std::pmr::memory_resource* resource_;
    std::optional<HttpTransferEncodingValue> value_;
    bool unsupported_{false};
};

}  // namespace ruvia::detail
