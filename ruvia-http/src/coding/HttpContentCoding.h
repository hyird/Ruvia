#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"

namespace ruvia::detail {

struct HttpContentCodingFieldResultAccess final {
    [[nodiscard]] static HttpContentCodingFieldResult coding(
        std::pmr::vector<HttpContentCoding> values) noexcept {
        return HttpContentCodingFieldResult(std::move(values));
    }

    [[nodiscard]] static constexpr HttpContentCodingFieldResult unsupported() noexcept {
        return HttpContentCodingFieldResult(HttpUnsupportedContentCoding{});
    }

    [[nodiscard]] static constexpr HttpContentCodingFieldResult invalid() noexcept {
        return HttpContentCodingFieldResult(HttpInvalidContentCodingField{});
    }
};

// Accumulates list grammar across every Content-Encoding field line. Recipients
// ignore empty list members, while senders cannot generate them.
class HttpContentCodingFieldParser final {
public:
    explicit HttpContentCodingFieldParser(
        HttpFieldListRole role = HttpFieldListRole::kRecipient,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : role_(role),
          codings_(resource != nullptr ? resource : std::pmr::get_default_resource()) {}

    void update(std::string_view value);

    [[nodiscard]] HttpContentCodingFieldResult finish() &&;

private:
    HttpFieldListRole role_;
    std::pmr::vector<HttpContentCoding> codings_;
    bool unsupported_{false};
    bool invalid_{false};
};

[[nodiscard]] bool isValidHttpContentEncodingFieldValue(
    std::string_view value, HttpFieldListRole role) noexcept;

template <typename Headers>
[[nodiscard]] inline HttpContentCodingFieldResult httpContentCodingFromHeaders(
    const Headers& headers, std::pmr::memory_resource* resource) {
    HttpContentCodingFieldParser parser(HttpFieldListRole::kRecipient, resource);
    for (const auto& header : headers) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Content-Encoding")) {
            parser.update(header.value());
        }
    }
    return std::move(parser).finish();
}

}  // namespace ruvia::detail
