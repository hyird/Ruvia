#pragma once

#include "ruvia/http/detail/HeaderTokenUtils.h"
#include "ruvia/http/detail/HttpTransferCoding.h"

#include <cstdint>
#include <string_view>

namespace ruvia::detail {

enum class HttpTransferEncodingParseStatus : std::uint8_t {
    kOk,
    kMalformed,
    kUnsupported
};

// Incremental, allocation-free parser for the supported Transfer-Encoding
// sequence shared by HTTP/1 requests and body-bearing client responses. Field
// lines are one logical list, so state and ordering continue across repeats.
class HttpTransferEncodingState final {
public:
    [[nodiscard]] HttpTransferEncodingParseStatus parseField(
        std::string_view fieldValue) noexcept {
        auto status = HttpTransferEncodingParseStatus::kOk;
        bool sawItem = false;
        httpVisitCommaSeparatedQuotedItems(
            fieldValue,
            [this, &status, &sawItem](std::string_view item) noexcept {
                sawItem = true;
                // gzip, deflate, and chunked define no transfer-coding parameters.
                // A semicolon here is not a chunk extension; chunk extensions live
                // on each chunk-size line inside the body.
                if (item.empty() || item.find(';') != std::string_view::npos ||
                    finalChunked_) {
                    status = HttpTransferEncodingParseStatus::kMalformed;
                    return false;
                }
                if (httpAsciiEqualsIgnoreCase(item, "chunked")) {
                    finalChunked_ = true;
                    present_ = true;
                    return true;
                }
                if (codings_.count == kMaxTransferCodings) {
                    status = HttpTransferEncodingParseStatus::kUnsupported;
                    return false;
                }
                if (httpAsciiEqualsIgnoreCase(item, "gzip") ||
                    httpAsciiEqualsIgnoreCase(item, "x-gzip")) {
                    codings_.values[codings_.count++] = HttpTransferCoding::kGzip;
                    present_ = true;
                    return true;
                }
                if (httpAsciiEqualsIgnoreCase(item, "deflate")) {
                    codings_.values[codings_.count++] = HttpTransferCoding::kDeflate;
                    present_ = true;
                    return true;
                }
                status = HttpTransferEncodingParseStatus::kUnsupported;
                return false;
            });
        return status == HttpTransferEncodingParseStatus::kOk && !sawItem
            ? HttpTransferEncodingParseStatus::kMalformed
            : status;
    }

    [[nodiscard]] bool present() const noexcept {
        return present_;
    }

    [[nodiscard]] bool finalChunked() const noexcept {
        return finalChunked_;
    }

    [[nodiscard]] const HttpTransferCodings& codings() const noexcept {
        return codings_;
    }

private:
    HttpTransferCodings codings_;
    bool present_{false};
    bool finalChunked_{false};
};

}  // namespace ruvia::detail
