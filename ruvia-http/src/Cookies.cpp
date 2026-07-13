#include "ruvia/http/detail/SetCookiePlan.h"

#include "ruvia/http/detail/CookieValidation.h"
#include "ruvia/http/detail/HttpImfFixdate.h"
#include "ruvia/http/detail/HttpNumberFormat.h"

#include <charconv>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace ruvia::detail {

SetCookiePlan::SetCookiePlan(
    std::string_view name,
    std::string_view value,
    const CookieOptions& options)
    : name_(name),
      value_(value),
      path_(options.path),
      domain_(options.domain),
      prefixText_(options.prefix
          ? cookiePrefixText(*options.prefix)
          : std::string_view{}),
      priorityText_(options.priority
          ? cookiePriorityToken(*options.priority)
          : std::string_view{}),
      sameSiteText_(options.sameSite
          ? cookieSameSiteToken(*options.sameSite)
          : std::string_view{}),
      hasMaxAge_(options.maxAge.has_value()),
      httpOnly_(options.httpOnly),
      secure_(options.secure),
      partitioned_(options.partitioned) {
    validateCookie(name, value, options);

    if (options.expires.has_value()) {
        const auto expiresTime = std::chrono::system_clock::to_time_t(*options.expires);
        const auto utc = httpUtcTm(expiresTime);
        expiresSize_ = httpWriteImfFixdate(expiresBuffer_.data(), utc);
    }
    if (hasMaxAge_) {
        maxAgeValue_ = static_cast<std::uint64_t>(options.maxAge->count());
        maxAgeSize_ = httpUnsignedDecimalSize(maxAgeValue_);
    }

    size_ = prefixText_.size() + name_.size() + 1 + value_.size();
    if (!path_.empty()) {
        size_ += std::string_view("; Path=").size() + path_.size();
    }
    if (!domain_.empty()) {
        size_ += std::string_view("; Domain=").size() + domain_.size();
    }
    if (hasMaxAge_) {
        size_ += std::string_view("; Max-Age=").size() + maxAgeSize_;
    }
    if (expiresSize_ != 0) {
        size_ += std::string_view("; Expires=").size() + expiresSize_;
    }
    if (httpOnly_) {
        size_ += std::string_view("; HttpOnly").size();
    }
    if (secure_) {
        size_ += std::string_view("; Secure").size();
    }
    if (!sameSiteText_.empty()) {
        size_ += std::string_view("; SameSite=").size() + sameSiteText_.size();
    }
    if (!priorityText_.empty()) {
        size_ += std::string_view("; Priority=").size() + priorityText_.size();
    }
    if (partitioned_) {
        size_ += std::string_view("; Partitioned").size();
    }
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
