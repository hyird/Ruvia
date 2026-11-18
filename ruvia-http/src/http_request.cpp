#include "http_request.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include "decimal_text.h"
#include "http1_server_response.h"
#include "http_authority.h"
#include "http_cache.h"
#include "http_cookie.h"
#include "http_field_kind.h"
#include "http_field_text.h"
#include "http_field_validation.h"
#include "http_header.h"
#include "http_message_access.h"
#include "http_message_framing_fields.h"
#include "http_response.h"
#include "http_uri_syntax.h"
#include "url_encoding.h"

namespace ruvia {
http_request& http_request::operator=(http_request&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // Publish the whole request before an old descriptor's allocator can
    // reenter. The return reference is fixed before retired storage is freed.
    http_request retired(std::move(*this));
    std::destroy_at(this);
    std::construct_at(this, std::move(other));
    return *this;
}

std::optional<std::string_view> http_request::header(std::string_view name) const& noexcept {
    const auto kind = detail::classify_http_field(name);
    if (kind != detail::http_field_kind::other) {
        const auto known_slot = detail::http_field_kind_known_slot(kind);
        const auto index = cached_headers_[known_slot];
        if (index == 0 || index > headers_.size()) {
            return std::nullopt;
        }
        return headers_[index - 1].value();
    }

    for (std::size_t index = 0; index < headers_.size(); ++index) {
        if (headers_.kind_at(index) == detail::http_field_kind::other && http_ascii_equals_ignore_case(headers_[index].name(), name)) {
            return headers_[index].value();
        }
    }

    return std::nullopt;
}

http_unique_field_lookup http_request::unique_header(std::string_view name) const& noexcept {
    const auto kind = detail::classify_http_field(name);
    if (kind != detail::http_field_kind::other) {
        const auto known_slot = detail::http_field_kind_known_slot(kind);
        const auto index = cached_headers_[known_slot];
        if (index == 0 || index > headers_.size()) {
            return {};
        }
        if (headers_.has_repeated_known_slot(known_slot)) {
            return {.status_ = unique_lookup_status::repeated};
        }
        return {.status_ = unique_lookup_status::found, .value_ = headers_[index - 1].value()};
    }

    http_unique_field_lookup result;
    for (std::size_t index = 0; index < headers_.size() && result.status_ != unique_lookup_status::repeated; ++index) {
        if (headers_.kind_at(index) == detail::http_field_kind::other && http_ascii_equals_ignore_case(headers_[index].name(), name)) {
            result.add_occurrence(headers_[index].value());
        }
    }
    return result;
}

std::optional<std::string_view> http_request::last_raw_query_value(std::string_view raw_name) const& noexcept {
    std::optional<std::string_view> result;
    (void)visit_url_encoded_pairs(query_string_, [&](std::string_view name, std::string_view value) noexcept {
        if (name == raw_name) {
            result = value;
        }
        return true;
    });
    return result;
}

std::optional<std::string_view> http_request::cookie(std::string_view name) const& noexcept {
    // The first matching pair across field lines, in wire order, wins.
    std::optional<std::string_view> result;
    (void)detail::http_request_access::visit_known_headers(*this, detail::http_field_kind::cookie, [name, &result](std::string_view field_value) noexcept {
        http_visit_cookie_pairs(field_value, [name, &result](std::string_view key, std::string_view value) noexcept {
            if (key == name) {
                result = value;
                return false;
            }
            return true;
        });
        return !result.has_value();
    });
    return result;
}

namespace {

template <typename field_type>
[[nodiscard]] std::optional<std::size_t> trailer_alias_offset(std::span<const field_type> fields, const std::vector<http_header>& retained) noexcept {
    if constexpr (!std::is_same_v<field_type, http_header>) {
        return std::nullopt;
    } else {
        if (fields.empty() || retained.empty()) {
            return std::nullopt;
        }
        const auto first = reinterpret_cast<std::uintptr_t>(fields.data());
        const auto last = reinterpret_cast<std::uintptr_t>(fields.data() + fields.size());
        const auto begin = reinterpret_cast<std::uintptr_t>(retained.data());
        const auto end = reinterpret_cast<std::uintptr_t>(retained.data() + retained.size());
        if (first < end && begin < last && first >= begin && last <= end && ((first - begin) % sizeof(http_header)) == 0) {
            return (first - begin) / sizeof(http_header);
        }
        return std::nullopt;
    }
}

template <typename field_type>
[[nodiscard]] std::optional<http_request_trailer_error> append_trailer_fields(std::vector<http_header>& retained, http_field_section_budget& retained_budget, std::span<const field_type> fields) {
    auto budget = retained_budget;
    for (const auto& field : fields) {
        if (const auto error = detail::charge_http_request_trailer_field(budget, field.name(), field.value())) {
            return error;
        }
    }
    if (fields.empty()) {
        return std::nullopt;
    }
    const auto alias_offset = trailer_alias_offset(fields, retained);
    const auto count = fields.size();
    const auto checkpoint = retained.size();
    detail::reserve_geometric(retained, checkpoint + count);
    struct rollback_guard final {
        std::vector<http_header>& retained_;
        std::size_t checkpoint_;
        bool active_{true};
        ~rollback_guard() {
            if (active_) {
                retained_.erase(retained_.begin() + static_cast<std::ptrdiff_t>(checkpoint_), retained_.end());
            }
        }
    } rollback{retained, checkpoint};
    if (alias_offset.has_value()) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto& field = retained[*alias_offset + i];
            retained.push_back(http_header::copy_with_lowercase_name(field.name(), field.value()));
        }
    } else {
        for (const auto& field : fields) {
            retained.push_back(http_header::copy_with_lowercase_name(field.name(), field.value()));
        }
    }
    rollback.active_ = false;
    retained_budget = budget;
    return std::nullopt;
}

[[nodiscard]] std::optional<http_request_trailer_error> validate_received_trailers(http_field_section_budget& budget, std::span<const http_header> fields, std::size_t& retained_count) noexcept {
    retained_count = 0;
    for (const auto& field : fields) {
        const auto name = field.name();
        const auto value = field.value();
        if (name.empty() || name.front() == ':' || !detail::is_valid_binary_field_name(name) || !is_valid_http_header_value(value)) {
            return http_request_trailer_error::invalid_field;
        }
        if (!budget.add_field(name, value)) {
            return http_request_trailer_error::section_too_large;
        }
        if (!detail::is_forbidden_http_request_trailer_name(name)) {
            ++retained_count;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<http_request_trailer_error> http_request_trailers::append(std::string_view name, std::string_view value) {
    const http_header_view field{name, value};
    return append(std::span<const http_header_view>(&field, 1));
}

std::optional<http_request_trailer_error> http_request_trailers::append(std::span<const http_header_view> fields) {
    return append_trailer_fields(fields_, budget_, fields);
}

std::optional<http_request_trailer_error> http_request_trailers::append(std::span<const http_header> fields) {
    return append_trailer_fields(fields_, budget_, fields);
}

std::optional<http_request_trailer_error> http_request_trailers::append_received(std::span<const http_header> fields) {
    auto budget = budget_;
    std::size_t retained_count = 0;
    if (const auto error = validate_received_trailers(budget, fields, retained_count)) {
        return error;
    }
    if (retained_count == 0) {
        budget_ = budget;
        return std::nullopt;
    }
    const auto alias_offset = trailer_alias_offset(fields, fields_);
    const auto count = fields.size();
    const auto checkpoint = fields_.size();
    detail::reserve_geometric(fields_, checkpoint + retained_count);
    struct rollback_guard final {
        std::vector<http_header>& retained_;
        std::size_t checkpoint_;
        bool active_{true};
        ~rollback_guard() {
            if (active_) {
                retained_.erase(retained_.begin() + static_cast<std::ptrdiff_t>(checkpoint_), retained_.end());
            }
        }
    } rollback{fields_, checkpoint};
    if (alias_offset.has_value()) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto& field = fields_[*alias_offset + i];
            if (!detail::is_forbidden_http_request_trailer_name(field.name())) {
                fields_.emplace_back(field.name(), field.value());
            }
        }
    } else {
        for (const auto& field : fields) {
            if (!detail::is_forbidden_http_request_trailer_name(field.name())) {
                fields_.emplace_back(field.name(), field.value());
            }
        }
    }
    rollback.active_ = false;
    budget_ = budget;
    return std::nullopt;
}

std::optional<http_request_trailer_error> http_request_trailers::append_received_owned(std::vector<http_header>&& fields) {
    auto budget = budget_;
    std::size_t retained_count = 0;
    if (const auto error = validate_received_trailers(budget, fields, retained_count)) {
        return error;
    }
    if (retained_count == 0) {
        budget_ = budget;
        return std::nullopt;
    }
    std::erase_if(fields, [](const http_header& field) noexcept {
        return detail::is_forbidden_http_request_trailer_name(field.name());
    });
    if (fields_.empty()) {
        fields_ = std::move(fields);
    } else {
        detail::reserve_geometric(fields_, fields_.size() + fields.size());
        for (auto& field : fields) {
            fields_.push_back(std::move(field));
        }
    }
    budget_ = budget;
    return std::nullopt;
}

std::optional<std::string_view> http_request_trailers::field(std::string_view name) const& noexcept {
    return find_first_http_header(fields_, name);
}

}  // namespace ruvia
