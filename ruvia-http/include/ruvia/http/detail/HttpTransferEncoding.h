#pragma once

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpTransferCoding.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace ruvia::detail {

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
                // gzip, deflate, and chunked define no transfer-coding parameters.
                // A semicolon here is not a chunk extension; chunk extensions live
                // on each chunk-size line inside the body.
                if (item.empty() || item.find(';') != std::string_view::npos ||
                    finalChunked) {
                    status = HttpTransferEncodingParseStatus::kMalformed;
                    return false;
                }
                if (httpAsciiEqualsIgnoreCase(item, "chunked")) {
                    finalChunked = true;
                    return true;
                }
                if (codings.count == kMaxTransferCodings) {
                    status = HttpTransferEncodingParseStatus::kUnsupported;
                    return false;
                }
                if (httpAsciiEqualsIgnoreCase(item, "gzip") ||
                    httpAsciiEqualsIgnoreCase(item, "x-gzip")) {
                    codings.values[codings.count++] = HttpTransferCoding::kGzip;
                    return true;
                }
                if (httpAsciiEqualsIgnoreCase(item, "deflate")) {
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
