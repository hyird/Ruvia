#include "ruvia/http/cookies.h"

#include <cassert>
#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/http/http_set_cookie_plan.h"

#include "cookie/cookie_validation.h"
#include "field/http_imf_fixdate.h"
#include "util/http_number_format.h"

namespace ruvia {

set_cookie_plan::set_cookie_plan(
    std::string_view name, std::string_view value, const cookie_options& options)
    : name_(name),
      value_(value),
      path_(options.path_),
      domain_(options.domain_),
      prefix_text_(options.prefix_ ? detail::cookie_prefix_text(*options.prefix_) : std::string_view{}),
      priority_text_(options.priority_ ? detail::cookie_priority_token(*options.priority_) : std::string_view{}),
      same_site_text_(options.same_site_ ? detail::cookie_same_site_token(*options.same_site_) : std::string_view{}),
      has_max_age_(options.max_age_.has_value()),
      http_only_(detail::cookie_attribute_emitted(options.http_only_)),
      secure_(detail::cookie_attribute_emitted(options.secure_)),
      partitioned_(detail::cookie_attribute_emitted(options.partitioned_)) {
    if (options.expires_.has_value()) {
        const auto seconds = std::chrono::floor<std::chrono::seconds>(options.expires_->time_since_epoch()).count();
        if (!std::in_range<std::time_t>(seconds)) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        const auto date = detail::http_format_date(static_cast<std::time_t>(seconds));
        if ((date.index() != 0)) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        expires_size_ = std::get<0>(date).size();
        std::memcpy(expires_buffer_.data(), std::get<0>(date).data(), expires_size_);
    }
    if (has_max_age_) {
        max_age_value_ = static_cast<std::uint64_t>(options.max_age_->count());
        max_age_size_ = detail::http_unsigned_decimal_size(max_age_value_);
    }

    const auto add_size = [this](std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - size_) {
            throw std::length_error("Set-Cookie value is too large");
        }
        size_ += amount;
    };

    // Compute the complete wire length before validating the borrowed views.
    // This keeps a hostile oversized view from reaching a grammar scan and
    // prevents the later uninitialized response-header write from receiving a
    // wrapped length.
    add_size(prefix_text_.size());
    add_size(name_.size());
    add_size(1);
    add_size(value_.size());
    if (!path_.empty()) {
        add_size(std::string_view("; Path=").size());
        add_size(path_.size());
    }
    if (!domain_.empty()) {
        add_size(std::string_view("; Domain=").size());
        add_size(domain_.size());
    }
    if (has_max_age_) {
        add_size(std::string_view("; Max-Age=").size());
        add_size(max_age_size_);
    }
    if (expires_size_ != 0) {
        add_size(std::string_view("; Expires=").size());
        add_size(expires_size_);
    }
    if (http_only_) {
        add_size(std::string_view("; HttpOnly").size());
    }
    if (secure_) {
        add_size(std::string_view("; Secure").size());
    }
    if (!same_site_text_.empty()) {
        add_size(std::string_view("; SameSite=").size());
        add_size(same_site_text_.size());
    }
    if (!priority_text_.empty()) {
        add_size(std::string_view("; Priority=").size());
        add_size(priority_text_.size());
    }
    if (partitioned_) {
        add_size(std::string_view("; Partitioned").size());
    }

    detail::validate_cookie(name, value, options);
}

void set_cookie_plan::write(char* cursor_value) const {
    (void)write_fields(cursor_value);
}

set_cookie_plan::written_fields set_cookie_plan::write_fields(char* cursor_value) const noexcept {
    written_fields fields;
    const auto* const name_begin = cursor_value;
    const auto append = [&cursor_value](std::string_view text) noexcept {
        if (!text.empty()) {
            std::memcpy(cursor_value, text.data(), text.size());
            cursor_value += text.size();
        }
    };
    const auto append_unsigned = [&cursor_value](std::uint64_t number, std::size_t size) noexcept {
        auto* const end = cursor_value + size;
        // The constructor fixed the exact digit count; integral to_chars does
        // not throw and this immutable value fits its prevalidated slice.
        const auto result_value = std::to_chars(cursor_value, end, number);
        assert(result_value.ec == std::errc{} && result_value.ptr == end);
        cursor_value = result_value.ptr;
    };

    append(prefix_text_);
    append(name_);
    fields.wire_name_ = {name_begin, static_cast<std::size_t>(cursor_value - name_begin)};
    *cursor_value++ = '=';
    append(value_);
    if (!path_.empty()) {
        append("; Path=");
        fields.path_ = {cursor_value, path_.size()};
        append(path_);
    }
    if (!domain_.empty()) {
        append("; Domain=");
        fields.domain_ = {cursor_value, domain_.size()};
        append(domain_);
    }
    if (has_max_age_) {
        append("; Max-Age=");
        append_unsigned(max_age_value_, max_age_size_);
    }
    if (expires_size_ != 0) {
        append("; Expires=");
        append(std::string_view(expires_buffer_.data(), expires_size_));
    }
    if (http_only_) {
        append("; HttpOnly");
    }
    if (secure_) {
        append("; Secure");
    }
    if (!same_site_text_.empty()) {
        append("; SameSite=");
        append(same_site_text_);
    }
    if (!priority_text_.empty()) {
        append("; Priority=");
        append(priority_text_);
    }
    if (partitioned_) {
        append("; Partitioned");
    }
    return fields;
}

}  // namespace ruvia

namespace ruvia {

bool is_valid_cookie_value(std::string_view value) noexcept {
    return detail::is_valid_cookie_value(value);
}

bool cookie_name_starts_with_ignore_case(std::string_view name, std::string_view prefix) noexcept {
    return detail::cookie_name_starts_with_ignore_case(name, prefix);
}

std::string_view http_cookie_prefix_text(cookie_prefix prefix) noexcept {
    return detail::cookie_prefix_text(prefix);
}

}  // namespace ruvia
