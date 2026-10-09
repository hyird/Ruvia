#include "client/client_request_policy.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_set_cookie.h"

#include "client/client_transport.h"
#include "client/http_client_config_storage.h"
#include "client/http_client_request_storage.h"

namespace ruvia::detail {
namespace {

bool cookie_domain_matches(std::string_view host, std::string_view domain) noexcept {
    if (domain.empty()) {
        return true;
    }
    if (http_ascii_equals_ignore_case(host, domain)) {
        return true;
    }
    if (is_client_ip_address(host)) {
        return false;
    }
    return host.size() > domain.size() && host[host.size() - domain.size() - 1] == '.' &&
           http_ascii_equals_ignore_case(host.substr(host.size() - domain.size()), domain);
}

bool cookie_path_matches(std::string_view request_path, std::string_view cookie_path) noexcept {
    if (cookie_path.empty() || cookie_path == "/") {
        return !request_path.empty() && request_path.front() == '/';
    }
    if (!request_path.starts_with(cookie_path)) {
        return false;
    }
    return request_path.size() == cookie_path.size() || cookie_path.back() == '/' ||
           request_path[cookie_path.size()] == '/';
}

bool is_valid_received_cookie_request_value(std::string_view value) noexcept {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return ::ruvia::is_valid_cookie_value(value);
}

bool can_serialize_received_cookie(std::string_view name, std::string_view value) noexcept {
    return (name.empty() || is_valid_http_header_name(name)) &&
           is_valid_received_cookie_request_value(value);
}

std::string_view request_path_only(std::string_view target) noexcept {
    return target.substr(0, target.find_first_of("?#"));
}

std::string_view default_cookie_path(std::string_view target) noexcept {
    const auto path = request_path_only(target);
    if (path.empty() || path.front() != '/') {
        return "/";
    }
    const auto slash = path.rfind('/');
    return slash == 0 || slash == std::string_view::npos ? std::string_view("/")
                                                         : path.substr(0, slash);
}

std::chrono::system_clock::time_point cookie_expiration(
    std::chrono::system_clock::time_point now, std::int64_t max_age_seconds) noexcept {
    using clock_type = std::chrono::system_clock;
    const std::chrono::duration<long double> requested{std::chrono::seconds(max_age_seconds)};
    const std::chrono::duration<long double> available{clock_type::time_point::max() - now};
    if (requested >= available) {
        return clock_type::time_point::max();
    }
    return now + std::chrono::duration_cast<clock_type::duration>(std::chrono::seconds(max_age_seconds));
}

}  // namespace

client_request_policy::client_request_policy(const http_client_config_storage& config,
    std::pmr::memory_resource* resource)
    : config_(config),
      resource_(resource),
      cookies_(resource) {
    for (const auto& [name, value] : config.cookies_) {
        add_cookie(name, value);
    }
}

void client_request_policy::add_cookie(std::string_view name, std::string_view value) {
    if (!is_valid_http_header_name(name) || !::ruvia::is_valid_cookie_value(value)) {
        throw std::invalid_argument("invalid HTTP client cookie");
    }
    const auto match = std::ranges::find_if(cookies_, [name](const stored_cookie& cookie) {
        return cookie.persistent_ && cookie.name_ == name && cookie.path_ == "/" &&
               cookie.domain_.empty();
    });
    const auto replacement_bytes = storage_bytes(name, value, "/", {});
    const auto replaced_bytes =
        match == cookies_.end()
            ? 0
            : storage_bytes(match->name_, match->value_, match->path_, match->domain_);
    if (!has_capacity(replaced_bytes, replacement_bytes, match == cookies_.end())) {
        throw std::length_error("HTTP client cookie jar capacity exceeded");
    }
    if (match == cookies_.end()) {
        reserve_cookie_slot();
        cookies_.emplace_back(name, value, resource_);
    } else {
        std::pmr::string replacement_value(value, resource_);
        match->value_.swap(replacement_value);
    }
    cookie_bytes_ = cookie_bytes_ - replaced_bytes + replacement_bytes;
}

std::size_t client_request_policy::storage_bytes(std::string_view name, std::string_view value,
    std::string_view path, std::string_view domain) noexcept {
    std::size_t total = 0;
    for (const auto field : {name, value, path, domain}) {
        if (field.size() > std::numeric_limits<std::size_t>::max() - total) {
            return std::numeric_limits<std::size_t>::max();
        }
        total += field.size();
    }
    return total;
}

bool client_request_policy::has_capacity(
    std::size_t replaced_bytes, std::size_t replacement_bytes, bool adding) const noexcept {
    if (adding && cookies_.size() >= config_.max_cookies_) {
        return false;
    }
    if (replaced_bytes > cookie_bytes_) {
        return false;
    }
    const auto retained_bytes = cookie_bytes_ - replaced_bytes;
    return replacement_bytes <=
           config_.max_cookie_bytes_ - std::min(retained_bytes, config_.max_cookie_bytes_);
}

void client_request_policy::reserve_cookie_slot() {
    if (cookies_.size() < cookies_.capacity()) {
        return;
    }
    // Capacity was checked before insertion. Grow geometrically without
    // retaining slots beyond the configured cookie count.
    const auto remaining = config_.max_cookies_ - cookies_.size();
    const auto growth = std::min(remaining, std::max(std::size_t{1}, cookies_.size() / 2));
    cookies_.reserve(cookies_.size() + growth);
}

void client_request_policy::discard_expired(std::chrono::system_clock::time_point now) {
    std::erase_if(cookies_, [this, now](const stored_cookie& cookie) {
        if (!cookie.expires_.has_value() || cookie.expires_.value() > now) {
            return false;
        }
        cookie_bytes_ -= storage_bytes(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
        return true;
    });
}

void client_request_policy::append_headers(const http_client_request_storage& request,
    std::pmr::vector<http_header_view>& headers, std::pmr::string& cookie_header) {
    const auto has_header = [&headers](std::string_view name) {
        return std::ranges::any_of(headers,
            [name](const http_header_view& header_value) { return http_ascii_equals_ignore_case(header_value.name(), name); });
    };
    if (!config_.user_agent_.empty() && !has_header("user-agent")) {
        headers.emplace_back("user-agent", config_.user_agent_);
    }
    std::erase_if(headers, [&cookie_header](const http_header_view& header_value) {
        if (!http_ascii_equals_ignore_case(header_value.name(), "cookie")) {
            return false;
        }
        if (!cookie_header.empty()) {
            cookie_header.append("; ");
        }
        cookie_header.append(header_value.value());
        return true;
    });

    discard_expired(std::chrono::system_clock::now());
    const auto path = request_path_only(request.target());
    for (const auto& cookie : cookies_) {
        if (!cookie.persistent_ &&
            config_.received_cookies_ == http_client_received_cookie_policy::ignore) {
            continue;
        }
        if (cookie.secure_ && config_.scheme_ != http_scheme::https) {
            continue;
        }
        if (!cookie_domain_matches(config_.host_, cookie.domain_) ||
            !cookie_path_matches(path, cookie.path_)) {
            continue;
        }
        append_cookie_request_pair(cookie_header, cookie.name_, cookie.value_);
    }
    if (!cookie_header.empty()) {
        headers.emplace_back("cookie", cookie_header);
    }
}

void client_request_policy::retain_response_cookies(
    const http_client_request_storage& request, std::span<const http_header> headers) {
    if (config_.received_cookies_ == http_client_received_cookie_policy::ignore) {
        return;
    }
    const auto now = std::chrono::system_clock::now();
    discard_expired(now);
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "set-cookie")) {
            continue;
        }
        const auto parsed_cookie = parse_set_cookie(header.value());
        if (!parsed_cookie) {
            continue;
        }
        const auto parsed_name = parsed_cookie->name();
        const auto parsed_value = parsed_cookie->value();
        const auto parsed_path = parsed_cookie->path();
        const auto parsed_domain = parsed_cookie->domain();
        const bool parsed_secure = parsed_cookie->has(http_set_cookie_attribute::secure);
        const bool parsed_has_path = parsed_cookie->has(http_set_cookie_attribute::path);
        const bool parsed_same_site_none = parsed_cookie->has(http_set_cookie_attribute::same_site_none);
        if ((parsed_secure && config_.scheme_ != http_scheme::https) ||
            !cookie_domain_matches(config_.host_, parsed_domain) ||
            !can_serialize_received_cookie(parsed_name, parsed_value)) {
            continue;
        }

        const auto path = parsed_path.empty() || parsed_path.front() != '/'
                              ? default_cookie_path(request.target())
                              : parsed_path;
        const bool secure_prefixed = ::ruvia::cookie_name_starts_with_ignore_case(parsed_name, "__Secure-");
        const bool host_prefixed = ::ruvia::cookie_name_starts_with_ignore_case(parsed_name, "__Host-");
        const bool nameless_prefix =
            parsed_name.empty() && (::ruvia::cookie_name_starts_with_ignore_case(parsed_value, "__Secure-") ||
                                       ::ruvia::cookie_name_starts_with_ignore_case(parsed_value, "__Host-"));
        if (nameless_prefix || (parsed_same_site_none && !parsed_secure) ||
            (secure_prefixed && (!parsed_secure || config_.scheme_ != http_scheme::https)) ||
            (host_prefixed && (!parsed_secure || config_.scheme_ != http_scheme::https ||
                                  !parsed_has_path || parsed_path != "/" || !parsed_domain.empty()))) {
            continue;
        }
        std::optional<std::chrono::system_clock::time_point> expires;
        bool remove = false;
        const auto max_age_seconds = parsed_cookie->max_age_seconds();
        const auto expires_at = parsed_cookie->expires();
        if (max_age_seconds) {
            remove = *max_age_seconds <= 0;
            if (!remove) {
                expires = cookie_expiration(now, *max_age_seconds);
            }
        } else if (expires_at) {
            remove = *expires_at <= std::chrono::system_clock::to_time_t(now);
            if (!remove) {
                const auto expiration_limit = std::chrono::system_clock::to_time_t(
                    cookie_expiration(now, max_cookie_age_seconds));
                expires =
                    std::chrono::system_clock::from_time_t(std::min(*expires_at, expiration_limit));
            }
        }
        const auto parsed_identity_domain =
            parsed_domain.empty() ? std::string_view(config_.host_) : parsed_domain;
        const bool parsed_host_only = parsed_domain.empty();
        const auto match = std::ranges::find_if(cookies_, [&](const stored_cookie& cookie) {
            const auto cookie_identity_domain = cookie.domain_.empty()
                                                    ? std::string_view(config_.host_)
                                                    : std::string_view(cookie.domain_);
            return !cookie.persistent_ && cookie.name_ == parsed_name &&
                   cookie.host_only_ == parsed_host_only && cookie.path_ == path &&
                   http_ascii_equals_ignore_case(cookie_identity_domain, parsed_identity_domain);
        });
        if (remove) {
            if (match != cookies_.end()) {
                cookie_bytes_ -=
                    storage_bytes(match->name_, match->value_, match->path_, match->domain_);
                cookies_.erase(match);
            }
            continue;
        }
        const auto replacement_bytes =
            storage_bytes(parsed_name, parsed_value, path, parsed_domain);
        const auto replaced_bytes =
            match == cookies_.end()
                ? 0
                : storage_bytes(match->name_, match->value_, match->path_, match->domain_);
        if (!has_capacity(replaced_bytes, replacement_bytes, match == cookies_.end())) {
            continue;
        }
        auto make_stored_cookie = [&]() {
            stored_cookie cookie(parsed_name, parsed_value, resource_);
            cookie.path_.assign(path);
            cookie.domain_.assign(parsed_domain);
            cookie.expires_ = expires;
            cookie.secure_ = parsed_secure;
            cookie.host_only_ = parsed_host_only;
            cookie.persistent_ = false;
            return cookie;
        };
        if (match == cookies_.end()) {
            const auto insertion = std::ranges::find_if(cookies_,
                [path](const stored_cookie& cookie) { return cookie.path_.size() < path.size(); });
            const auto insertion_index = static_cast<std::size_t>(insertion - cookies_.begin());
            auto cookie = make_stored_cookie();
            reserve_cookie_slot();
            cookies_.emplace(
                cookies_.begin() + static_cast<std::ptrdiff_t>(insertion_index), std::move(cookie));
        } else {
            auto replacement = make_stored_cookie();
            match->name_.swap(replacement.name_);
            match->value_.swap(replacement.value_);
            match->path_.swap(replacement.path_);
            match->domain_.swap(replacement.domain_);
            std::swap(match->expires_, replacement.expires_);
            std::swap(match->secure_, replacement.secure_);
            std::swap(match->host_only_, replacement.host_only_);
            std::swap(match->persistent_, replacement.persistent_);
        }
        cookie_bytes_ = cookie_bytes_ - replaced_bytes + replacement_bytes;
    }
}

}  // namespace ruvia::detail
