#include "ruvia/http/http_request_trailers.h"

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_syntax.h"

namespace ruvia {
http_request_trailers::http_request_trailers(std::pmr::memory_resource* resource)
    : fields_(resource != nullptr ? resource : std::pmr::get_default_resource()) {}

std::optional<http_request_trailer_error> http_request_trailers::append(std::string_view name, std::string_view value) {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return http_request_trailer_error::invalid_field;
    }
    if (detail::is_forbidden_http_request_trailer_name(name)) {
        return http_request_trailer_error::forbidden_field;
    }
    auto budget = budget_;
    if (!budget.add_field(name, value)) {
        return http_request_trailer_error::section_too_large;
    }
    fields_.push_back(
        http_header::copy_with_lowercase_name(name, value, fields_.get_allocator().resource()));
    budget_ = budget;
    return std::nullopt;
}

std::optional<http_request_trailer_error> http_request_trailers::append_http1(std::string_view block) {
    if (block.ends_with("\r\n\r\n")) {
        block.remove_suffix(4);
    } else if (block.ends_with("\r\n")) {
        block.remove_suffix(2);
    }
    // The block is appended all-or-nothing, like append(): it extends the
    // retained section's field count and byte budget. Received fields that are
    // not permitted in request trailers are discarded (RFC 9112 section 7.1.2).
    const auto field_count = fields_.size();
    auto budget = budget_;
    std::size_t retained_count = 0;
    const auto preflight_status = detail::visit_http1_trailer_block(
        block, detail::is_forbidden_http_request_trailer_name, budget,
        [&retained_count](std::string_view, std::string_view) noexcept {
            ++retained_count;
            return true;
        });
    switch (preflight_status) {
        case detail::http_trailer_block_status::ok:
            break;
        case detail::http_trailer_block_status::section_too_large:
            return http_request_trailer_error::section_too_large;
        case detail::http_trailer_block_status::invalid_field:
        case detail::http_trailer_block_status::visitor_stopped:
            return http_request_trailer_error::invalid_field;
    }
    if (retained_count == 0) {
        budget_ = budget;
        return std::nullopt;
    }
    const auto roll_back = [&]() noexcept {
        fields_.erase(fields_.begin() + static_cast<std::ptrdiff_t>(field_count), fields_.end());
    };
    try {
        fields_.reserve(field_count + retained_count);
        auto append_budget = budget_;
        (void)detail::visit_http1_trailer_block(
            block, detail::is_forbidden_http_request_trailer_name, append_budget,
            [this](std::string_view name, std::string_view value) {
                fields_.emplace_back(name, value, fields_.get_allocator().resource());
                return true;
            });
        budget_ = budget;
        return std::nullopt;
    } catch (...) {
        roll_back();
        throw;
    }
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
