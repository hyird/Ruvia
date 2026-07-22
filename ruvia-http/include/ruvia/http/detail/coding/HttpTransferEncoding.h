#pragma once

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/coding/HttpTransferCoding.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace ruvia::detail {

[[nodiscard]] inline bool httpValidTransferParameterValue(
    std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    if (value.front() != '"') {
        return std::ranges::all_of(value, [](char byte) noexcept {
            return isHttpTokenChar(static_cast<unsigned char>(byte));
        });
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
            if (byte != '\t' && byte != ' ' &&
                (byte < 0x21 || byte > 0x7e) && byte < 0x80) {
                return false;
            }
        } else if (byte == '"' ||
                   (byte != '\t' && byte != ' ' && byte != 0x21 &&
                    (byte < 0x23 || byte > 0x5b) &&
                    (byte < 0x5d || byte > 0x7e) && byte < 0x80)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool httpParseTransferCodingSyntax(
    std::string_view item,
    std::string_view& coding,
    bool& hasParameters) noexcept {
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

enum class HttpTransferEncodingParseStatus : std::uint8_t {
    kOk,
    kMalformed,
    kUnsupported
};

class HttpNonChunkedTransferEncoding final {
public:
    [[nodiscard]] HttpTransferCodings transferCodings() const noexcept {
        return transferCodings_;
    }

private:
    friend class HttpTransferEncodingValue;

    explicit HttpNonChunkedTransferEncoding(
        HttpTransferCodings transferCodings) noexcept
        : transferCodings_(transferCodings) {}

    HttpTransferCodings transferCodings_;
};

class HttpFinalChunkedTransferEncoding final {
public:
    [[nodiscard]] HttpTransferCodings transferCodings() const noexcept {
        return transferCodings_;
    }

private:
    friend class HttpTransferEncodingValue;

    explicit HttpFinalChunkedTransferEncoding(
        HttpTransferCodings transferCodings) noexcept
        : transferCodings_(transferCodings) {}

    HttpTransferCodings transferCodings_;
};

class HttpTransferEncodingValue final {
public:
    [[nodiscard]] const HttpNonChunkedTransferEncoding*
    nonChunked() const & noexcept {
        return std::get_if<HttpNonChunkedTransferEncoding>(&value_);
    }
    const HttpNonChunkedTransferEncoding* nonChunked() const && = delete;

    [[nodiscard]] const HttpFinalChunkedTransferEncoding*
    finalChunked() const & noexcept {
        return std::get_if<HttpFinalChunkedTransferEncoding>(&value_);
    }
    const HttpFinalChunkedTransferEncoding* finalChunked() const && = delete;

private:
    friend class HttpTransferEncodingState;

    using Value = std::variant<
        HttpNonChunkedTransferEncoding,
        HttpFinalChunkedTransferEncoding>;

    [[nodiscard]] static HttpTransferEncodingValue makeNonChunked(
        HttpTransferCodings transferCodings) noexcept {
        return HttpTransferEncodingValue(
            HttpNonChunkedTransferEncoding(transferCodings));
    }

    [[nodiscard]] static HttpTransferEncodingValue makeFinalChunked(
        HttpTransferCodings transferCodings) noexcept {
        return HttpTransferEncodingValue(
            HttpFinalChunkedTransferEncoding(transferCodings));
    }

    explicit HttpTransferEncodingValue(
        HttpNonChunkedTransferEncoding value) noexcept
        : value_(value) {}

    explicit HttpTransferEncodingValue(
        HttpFinalChunkedTransferEncoding value) noexcept
        : value_(value) {}

    Value value_;
};

// Incremental, allocation-free parser for the supported Transfer-Encoding
// sequence shared by HTTP/1 requests and body-bearing client responses. Field
// lines are one logical list, so state and ordering continue across repeats.
// Each field update is transactional. The exposed optional discriminates an
// absent field, a non-chunked coding sequence, and a final-chunked sequence.
class HttpTransferEncodingState final {
public:
    [[nodiscard]] HttpTransferEncodingParseStatus parseField(
        std::string_view fieldValue) noexcept {
        auto next = value_;
        HttpTransferCodings codings;
        bool finalChunked = false;
        if (next.has_value()) {
            if (const auto* final = next->finalChunked()) {
                codings = final->transferCodings();
                finalChunked = true;
            } else {
                codings = next->nonChunked()->transferCodings();
            }
        }

        auto status = HttpTransferEncodingParseStatus::kOk;
        bool sawItem = false;
        httpVisitCommaSeparatedQuotedItems(
            fieldValue,
            [&codings, &finalChunked, &status, &sawItem](
                std::string_view item) noexcept {
                sawItem = true;
                std::string_view coding;
                bool hasParameters = false;
                if (finalChunked ||
                    !httpParseTransferCodingSyntax(item, coding, hasParameters)) {
                    status = HttpTransferEncodingParseStatus::kMalformed;
                    return false;
                }
                // RFC 9112 section 7.2: chunked and the compression transfer
                // codings define no parameters. Unknown codings can define them,
                // so valid extension syntax remains "unsupported", not malformed.
                if (httpAsciiEqualsIgnoreCase(coding, "chunked")) {
                    if (hasParameters) {
                        status = HttpTransferEncodingParseStatus::kMalformed;
                        return false;
                    }
                    finalChunked = true;
                    return true;
                }
                if (codings.count == kMaxTransferCodings) {
                    status = HttpTransferEncodingParseStatus::kUnsupported;
                    return false;
                }
                if (httpAsciiEqualsIgnoreCase(coding, "gzip") ||
                    httpAsciiEqualsIgnoreCase(coding, "x-gzip")) {
                    if (hasParameters) {
                        status = HttpTransferEncodingParseStatus::kMalformed;
                        return false;
                    }
                    codings.values[codings.count++] = HttpTransferCoding::kGzip;
                    return true;
                }
                if (httpAsciiEqualsIgnoreCase(coding, "deflate")) {
                    if (hasParameters) {
                        status = HttpTransferEncodingParseStatus::kMalformed;
                        return false;
                    }
                    codings.values[codings.count++] = HttpTransferCoding::kDeflate;
                    return true;
                }
                status = HttpTransferEncodingParseStatus::kUnsupported;
                return false;
            });
        if (status == HttpTransferEncodingParseStatus::kOk && !sawItem) {
            return HttpTransferEncodingParseStatus::kMalformed;
        }
        if (status == HttpTransferEncodingParseStatus::kOk) {
            value_ = finalChunked
                ? HttpTransferEncodingValue::makeFinalChunked(codings)
                : HttpTransferEncodingValue::makeNonChunked(codings);
        }
        return status;
    }

    [[nodiscard]] std::optional<HttpTransferEncodingValue>
    value() const noexcept {
        return value_;
    }

private:
    std::optional<HttpTransferEncodingValue> value_;
};

}  // namespace ruvia::detail
