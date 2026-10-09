#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http_content_coding.h"

namespace ruvia::detail {

struct http_content_coding_field_result_access final {
    [[nodiscard]] static http_content_coding_field_result coding(
        std::pmr::vector<http_content_coding> values) noexcept {
        return http_content_coding_field_result(std::move(values));
    }

    [[nodiscard]] static constexpr http_content_coding_field_result unsupported() noexcept {
        return http_content_coding_field_result(http_unsupported_content_coding{});
    }

    [[nodiscard]] static constexpr http_content_coding_field_result invalid() noexcept {
        return http_content_coding_field_result(http_invalid_content_coding_field{});
    }
};

// Accumulates list grammar across every Content-Encoding field line. Recipients
// ignore empty list members, while senders cannot generate them.
class http_content_coding_field_parser final {
public:
    explicit http_content_coding_field_parser(
        http_field_list_role role = http_field_list_role::recipient,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : role_(role),
          codings_(resource != nullptr ? resource : std::pmr::get_default_resource()) {}

    void update(std::string_view value);

    [[nodiscard]] http_content_coding_field_result finish() &&;

private:
    http_field_list_role role_;
    std::pmr::vector<http_content_coding> codings_;
    bool unsupported_{false};
    bool invalid_{false};
};

[[nodiscard]] bool is_valid_http_content_encoding_field_value(
    std::string_view value, http_field_list_role role) noexcept;

template <typename headers_type>
[[nodiscard]] inline http_content_coding_field_result http_content_coding_from_headers(
    const headers_type& headers, std::pmr::memory_resource* resource) {
    http_content_coding_field_parser parser(http_field_list_role::recipient, resource);
    for (const auto& header : headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Content-Encoding")) {
            parser.update(header.value());
        }
    }
    return std::move(parser).finish();
}

}  // namespace ruvia::detail
