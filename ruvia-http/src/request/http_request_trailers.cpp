#include "ruvia/http/http_request_trailers.h"

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_ascii.h"

#include "request/http_request_trailer_field.h"

namespace ruvia {
namespace {

template <typename field_type>
[[nodiscard]] std::optional<http_request_trailer_error> append_trailer_fields(
    std::pmr::vector<http_header>& retained, http_field_section_budget& retained_budget,
    std::span<const field_type> fields) {
    auto budget = retained_budget;
    for (const auto& field : fields) {
        if (const auto error = detail::charge_http_request_trailer_field(budget, field.name(), field.value())) {
            return error;
        }
    }
    if (fields.empty()) {
        return std::nullopt;
    }
    const auto field_count = retained.size();
    const auto roll_back = [&]() noexcept {
        retained.erase(retained.begin() + static_cast<std::ptrdiff_t>(field_count), retained.end());
    };
    try {
        retained.reserve(field_count + fields.size());
        auto* const resource = retained.get_allocator().resource();
        for (const auto& field : fields) {
            retained.push_back(http_header::copy_with_lowercase_name(field.name(), field.value(), resource));
        }
        retained_budget = budget;
        return std::nullopt;
    } catch (...) {
        roll_back();
        throw;
    }
}

}  // namespace

http_request_trailers::http_request_trailers(std::pmr::memory_resource* resource)
    : fields_(detail::http_pmr_resource_or_default(resource)) {}

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

std::optional<http_request_trailer_error> http_request_trailers::append_http1(std::string_view block) {
    if (block.ends_with("\r\n\r\n")) {
        block.remove_suffix(4);
    } else if (block.ends_with("\r\n")) {
        block.remove_suffix(2);
    }
    // The block is appended all-or-nothing, like append(): it extends the
    // retained section's field count and byte budget, and names are retained
    // lowercase as append() and HTTP/2 retain them. Received fields that are
    // not permitted in request trailers are discarded (RFC 9112 section 7.1.2).
    // Fields are appended during the single validating pass and rolled back
    // when the block turns out invalid.
    const auto field_count = fields_.size();
    const auto roll_back = [&]() noexcept {
        fields_.erase(fields_.begin() + static_cast<std::ptrdiff_t>(field_count), fields_.end());
    };
    auto budget = budget_;
    auto status = detail::http_trailer_block_status::ok;
    try {
        auto* const resource = fields_.get_allocator().resource();
        status = detail::visit_http1_trailer_block(block, detail::is_forbidden_http_request_trailer_name, budget,
            [this, resource](std::string_view name, std::string_view value) {
                fields_.push_back(http_header::copy_with_lowercase_name(name, value, resource));
                return true;
            });
    } catch (...) {
        roll_back();
        throw;
    }
    switch (status) {
        case detail::http_trailer_block_status::ok:
            budget_ = budget;
            return std::nullopt;
        case detail::http_trailer_block_status::section_too_large:
            roll_back();
            return http_request_trailer_error::section_too_large;
        case detail::http_trailer_block_status::invalid_field:
        case detail::http_trailer_block_status::visitor_stopped:
            break;
    }
    roll_back();
    return http_request_trailer_error::invalid_field;
}

std::optional<std::string_view> http_request_trailers::field(std::string_view name) const& noexcept {
    for (auto field = fields_.rbegin(); field != fields_.rend(); ++field) {
        if (http_ascii_equals_ignore_case(field->name(), name)) {
            return field->value();
        }
    }
    return std::nullopt;
}
}  // namespace ruvia
