#include "ruvia/http/detail/cookie/SetCookiePlan.h"

#include "ruvia/http/detail/cookie/CookieValidation.h"
#include "ruvia/http/detail/field/HttpImfFixdate.h"
#include "ruvia/http/detail/util/HttpNumberFormat.h"

#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace ruvia::detail {

SetCookiePlan::SetCookiePlan(std::string_view name, std::string_view value, const CookieOptions& options)
    : name_(name),
      value_(value),
      path_(options.path),
      domain_(options.domain),
      prefixText_(options.prefix ? cookiePrefixText(*options.prefix) : std::string_view{}),
      priorityText_(options.priority ? cookiePriorityToken(*options.priority) : std::string_view{}),
      sameSiteText_(options.sameSite ? cookieSameSiteToken(*options.sameSite) : std::string_view{}),
      hasMaxAge_(options.maxAge.has_value()),
      httpOnly_(options.httpOnly),
      secure_(options.secure),
      partitioned_(options.partitioned) {
    if (options.expires.has_value()) {
        const auto expiresTime = std::chrono::system_clock::to_time_t(*options.expires);
        const auto utc = httpUtcTm(expiresTime);
        expiresSize_ = httpWriteImfFixdate(expiresBuffer_.data(), utc);
    }
    if (hasMaxAge_) {
        maxAgeValue_ = static_cast<std::uint64_t>(options.maxAge->count());
        maxAgeSize_ = httpUnsignedDecimalSize(maxAgeValue_);
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

    validateCookie(name, value, options);
}

void SetCookiePlan::write(char* cursor) const {
    const auto append = [&cursor](std::string_view text) noexcept {
        if (!text.empty()) {
            std::memcpy(cursor, text.data(), text.size());
            cursor += text.size();
        }
    };
    const auto appendUnsigned = [&cursor](std::uint64_t number, std::size_t size) {
        auto* const end = cursor + size;
        const auto [ptr, ec] = std::to_chars(cursor, end, number);
        if (ec != std::errc{} || ptr != end) {
            throw std::logic_error("failed to format cookie Max-Age");
        }
        cursor = ptr;
    };

    append(prefixText_);
    append(name_);
    *cursor++ = '=';
    append(value_);
    if (!path_.empty()) {
        append("; Path=");
        append(path_);
    }
    if (!domain_.empty()) {
        append("; Domain=");
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
}

}  // namespace ruvia::detail
