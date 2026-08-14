#pragma once

#include <cstddef>
#include <string_view>
#include <variant>

#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"

namespace ruvia::detail {

struct HttpContentCodingFieldResultAccess final {
    [[nodiscard]] static constexpr HttpContentCodingFieldResult coding(HttpContentCoding value) noexcept {
        return HttpContentCodingFieldResult(value);
    }

    [[nodiscard]] static constexpr HttpContentCodingFieldResult unsupported() noexcept {
        return HttpContentCodingFieldResult(HttpUnsupportedContentCoding{});
    }

    [[nodiscard]] static constexpr HttpContentCodingFieldResult invalid() noexcept {
        return HttpContentCodingFieldResult(HttpInvalidContentCodingField{});
    }
};

[[nodiscard]] inline constexpr std::string_view httpSupportedRequestContentCodings() noexcept {
    return "gzip, br, zstd";
}

// Accumulates list grammar across every Content-Encoding field line. Recipients
// ignore empty list members, while senders cannot generate them.
class HttpContentCodingFieldParser final {
public:
    explicit HttpContentCodingFieldParser(HttpFieldListRole role = HttpFieldListRole::kRecipient) noexcept
        : role_(role),
          state_(std::in_place_type<Supported>) {}

    void update(std::string_view value) noexcept;

    [[nodiscard]] HttpContentCodingFieldResult finish() const noexcept;

private:
    struct Supported final {
        HttpContentCoding coding{HttpContentCoding::kIdentity};
        std::size_t codingCount{0};
    };

    HttpFieldListRole role_;
    std::variant<Supported, HttpUnsupportedContentCoding, HttpInvalidContentCodingField> state_;
};

[[nodiscard]] bool isValidHttpContentEncodingFieldValue(std::string_view value, HttpFieldListRole role) noexcept;

template <typename Headers>
[[nodiscard]] inline HttpContentCodingFieldResult httpContentCodingFromHeaders(const Headers& headers) noexcept {
    HttpContentCodingFieldParser parser;
    for (const auto& header : headers) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Content-Encoding")) {
            parser.update(header.value());
        }
    }
    return parser.finish();
}

}  // namespace ruvia::detail
