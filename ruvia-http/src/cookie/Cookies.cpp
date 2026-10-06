#include "ruvia/http/Cookies.h"

#include <cassert>
#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "ruvia/http/HttpSetCookiePlan.h"
#include "ruvia/http/detail/cookie/CookieValidation.h"
#include "ruvia/http/detail/field/HttpImfFixdate.h"
#include "ruvia/http/detail/util/HttpNumberFormat.h"

namespace ruvia {

SetCookiePlan::SetCookiePlan(
    std::string_view name, std::string_view value, const CookieOptions& options)
    : name_(name),
      value_(value),
      path_(options.path),
      domain_(options.domain),
      prefixText_(options.prefix ? detail::cookiePrefixText(*options.prefix) : std::string_view{}),
      priorityText_(options.priority ? detail::cookiePriorityToken(*options.priority) : std::string_view{}),
      sameSiteText_(options.sameSite ? detail::cookieSameSiteToken(*options.sameSite) : std::string_view{}),
      hasMaxAge_(options.maxAge.has_value()),
      httpOnly_(detail::cookieAttributeEmitted(options.httpOnly)),
      secure_(detail::cookieAttributeEmitted(options.secure)),
      partitioned_(detail::cookieAttributeEmitted(options.partitioned)) {
    if (options.expires.has_value()) {
        const auto seconds = std::chrono::floor<std::chrono::seconds>(options.expires->time_since_epoch()).count();
        if (!std::in_range<std::time_t>(seconds)) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        const auto date = detail::httpFormatDate(static_cast<std::time_t>(seconds));
        if (!date) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        expiresSize_ = date->size();
        std::memcpy(expiresBuffer_.data(), date->data(), expiresSize_);
    }
    if (hasMaxAge_) {
        maxAgeValue_ = static_cast<std::uint64_t>(options.maxAge->count());
        maxAgeSize_ = detail::httpUnsignedDecimalSize(maxAgeValue_);
    }

    const auto addSize = [this](std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - size_) {
            throw std::length_error("Set-Cookie value is too large");
        }
        size_ += amount;
    };

    // Compute the complete wire length before validating the borrowed views.
    // This keeps a hostile oversized view from reaching a grammar scan and
    // prevents the later uninitialized response-header write from receiving a
    // wrapped length.
    addSize(prefixText_.size());
    addSize(name_.size());
    addSize(1);
    addSize(value_.size());
    if (!path_.empty()) {
        addSize(std::string_view("; Path=").size());
        addSize(path_.size());
    }
    if (!domain_.empty()) {
        addSize(std::string_view("; Domain=").size());
        addSize(domain_.size());
    }
    if (hasMaxAge_) {
        addSize(std::string_view("; Max-Age=").size());
        addSize(maxAgeSize_);
    }
    if (expiresSize_ != 0) {
        addSize(std::string_view("; Expires=").size());
        addSize(expiresSize_);
    }
    if (httpOnly_) {
        addSize(std::string_view("; HttpOnly").size());
    }
    if (secure_) {
        addSize(std::string_view("; Secure").size());
    }
    if (!sameSiteText_.empty()) {
        addSize(std::string_view("; SameSite=").size());
        addSize(sameSiteText_.size());
    }
    if (!priorityText_.empty()) {
        addSize(std::string_view("; Priority=").size());
        addSize(priorityText_.size());
    }
    if (partitioned_) {
        addSize(std::string_view("; Partitioned").size());
    }

    detail::validateCookie(name, value, options);
}

void SetCookiePlan::write(char* cursor) const {
    (void)write_fields(cursor);
}

SetCookiePlan::written_fields SetCookiePlan::write_fields(char* cursor) const noexcept {
    written_fields fields;
    const auto* const name_begin = cursor;
    const auto append = [&cursor](std::string_view text) noexcept {
        if (!text.empty()) {
            std::memcpy(cursor, text.data(), text.size());
            cursor += text.size();
        }
    };
    const auto appendUnsigned = [&cursor](std::uint64_t number, std::size_t size) noexcept {
        auto* const end = cursor + size;
        // The constructor fixed the exact digit count; integral to_chars does
        // not throw and this immutable value fits its prevalidated slice.
        const auto result = std::to_chars(cursor, end, number);
        assert(result.ec == std::errc{} && result.ptr == end);
        cursor = result.ptr;
    };

    append(prefixText_);
    append(name_);
    fields.wire_name_ = {name_begin, static_cast<std::size_t>(cursor - name_begin)};
    *cursor++ = '=';
    append(value_);
    if (!path_.empty()) {
        append("; Path=");
        fields.path_ = {cursor, path_.size()};
        append(path_);
    }
    if (!domain_.empty()) {
        append("; Domain=");
        fields.domain_ = {cursor, domain_.size()};
        append(domain_);
    }
    if (hasMaxAge_) {
        append("; Max-Age=");
        appendUnsigned(maxAgeValue_, maxAgeSize_);
    }
    if (expiresSize_ != 0) {
        append("; Expires=");
        append(std::string_view(expiresBuffer_.data(), expiresSize_));
    }
    if (httpOnly_) {
        append("; HttpOnly");
    }
    if (secure_) {
        append("; Secure");
    }
    if (!sameSiteText_.empty()) {
        append("; SameSite=");
        append(sameSiteText_);
    }
    if (!priorityText_.empty()) {
        append("; Priority=");
        append(priorityText_);
    }
    if (partitioned_) {
        append("; Partitioned");
    }
    return fields;
}

}  // namespace ruvia

namespace ruvia {

bool isValidCookieValue(std::string_view value) noexcept {
    return detail::isValidCookieValue(value);
}

bool cookieNameStartsWithIgnoreCase(std::string_view name, std::string_view prefix) noexcept {
    return detail::cookieNameStartsWithIgnoreCase(name, prefix);
}

std::string_view httpCookiePrefixText(CookiePrefix prefix) noexcept {
    return detail::cookiePrefixText(prefix);
}

}  // namespace ruvia
