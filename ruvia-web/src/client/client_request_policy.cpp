#include "client/client_request_policy.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_values.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_set_cookie.h"

#include "client/client_host.h"
#include "client/http_client_config_storage.h"
#include "client/http_client_request_storage.h"

namespace ruvia::detail {

// A Set-Cookie accepted for this origin. Views borrow the response header
// value and the request target.
struct client_received_cookie final {
    std::string_view name_;
    std::string_view value_;
    std::string_view path_;
    std::string_view domain_;
    std::optional<std::chrono::system_clock::time_point> expires_;
    bool secure_{false};
    // Max-Age <= 0 or an Expires in the past: deletes the stored match.
    bool expired_{false};
};

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

bool satisfies_name_prefix(const http_set_cookie_view& cookie, bool secure) noexcept {
    const auto prefix = ::ruvia::cookie_name_prefix(cookie.name());
    return !prefix || ::ruvia::cookie_prefix_requirements_met(*prefix, secure, cookie.path(), cookie.domain());
}

// RFC 6265 section 5.3 storage acceptance for this client's fixed origin.
// Secure cookies are only accepted over https, which also covers the secure
// origin that the __Secure-/__Host- prefixes require. parse_set_cookie already
// ignored nameless cookies and control bytes, so the stored name and value
// (including internal whitespace, RFC 6265 section 5.2) are sent back verbatim.
std::optional<client_received_cookie> accept_set_cookie(const http_set_cookie_view& parsed,
    const http_client_config_storage& config, std::string_view request_target,
    std::chrono::system_clock::time_point now) {
    const bool secure = parsed.has(http_set_cookie_attribute::secure);
    if ((secure && config.scheme_ != http_scheme::https) || !cookie_domain_matches(config.host_, parsed.domain()) ||
        (parsed.has(http_set_cookie_attribute::same_site_none) && !secure) ||
        (parsed.has(http_set_cookie_attribute::partitioned) && !secure) ||
        !satisfies_name_prefix(parsed, secure)) {
        return std::nullopt;
    }

    client_received_cookie cookie{
        .name_ = parsed.name(),
        .value_ = parsed.value(),
        .path_ = parsed.path().empty() ? default_cookie_path(request_target) : parsed.path(),
        .domain_ = parsed.domain(),
        .expires_ = std::nullopt,
        .secure_ = secure,
    };
    if (const auto max_age_seconds = parsed.max_age_seconds()) {
        cookie.expired_ = *max_age_seconds <= 0;
        if (!cookie.expired_) {
            cookie.expires_ = cookie_expiration(now, *max_age_seconds);
        }
    } else if (const auto expires_at = parsed.expires()) {
        cookie.expired_ = *expires_at <= std::chrono::system_clock::to_time_t(now);
        if (!cookie.expired_) {
            const auto expiration_limit =
                std::chrono::system_clock::to_time_t(cookie_expiration(now, max_cookie_age_seconds));
            cookie.expires_ = std::chrono::system_clock::from_time_t(std::min(*expires_at, expiration_limit));
        }
    }
    return cookie;
}

}  // namespace

client_request_policy::client_request_policy(const http_client_config_storage& config,
    std::pmr::memory_resource* resource)
    : config_(config),
      resource_(resource),
      cookies_(resource) {
    cookies_.reserve(config.cookies_.size());
    for (const auto& cookie : config.cookies_) {
        add_cookie(cookie.name(), cookie.value());
    }
}

void client_request_policy::add_cookie(std::string_view name, std::string_view value) {
    if (!is_valid_http_header_name(name) || !::ruvia::is_valid_cookie_value(value)) {
        throw std::invalid_argument("invalid HTTP client cookie");
    }
    const auto match = std::ranges::find_if(cookies_, [name](const stored_cookie& cookie) {
        return cookie.configured_ && cookie.name_ == name && cookie.path_ == "/" &&
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
    } else if (match->value_ != value) {
        if (value.size() <= match->value_.capacity()) {
            match->value_.assign(value);
        } else {
            std::pmr::string replacement_value(value, resource_);
            match->value_.swap(replacement_value);
        }
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
    const auto growth = std::min(remaining, std::max(std::size_t{4}, cookies_.size()));
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

http_client_request_view client_request_policy::prepare_request(
    const http_client_request_storage& request, client_prepared_request_scratch& scratch) {
    const auto& input_headers = request.headers();
    auto field_count = input_headers.size();
    bool add_user_agent = !config_.user_agent_.empty();
    scratch.overflow_cookie_header_.clear();
    std::size_t cookie_header_bytes = 0;
    // Request storage preserves caller field-name spelling.
    for (const auto& field : input_headers) {
        const auto name = field.name();
        if (add_user_agent && name.size() == 10U && http_ascii_equals_ignore_case(name, "user-agent")) {
            add_user_agent = false;
        }
        if (name.size() != 6U || !http_ascii_equals_ignore_case(name, "cookie")) {
            continue;
        }
        --field_count;
        if (cookie_header_bytes != 0) {
            cookie_header_bytes += 2;
        }
        cookie_header_bytes += field.value().size();
    }
    const bool has_input_cookie = field_count != input_headers.size();

    // RFC 9110 §9.3.8: TRACE reflects the request, so stored credentials stay
    // out; caller-written Cookie fields above remain the caller's choice.
    // Empty jars have no expiry or path constraints to inspect.
    const bool check_jar = !cookies_.empty() && request.method() != "TRACE";
    std::string_view path;
    if (check_jar) {
        discard_expired(std::chrono::system_clock::now());
        path = request_path_only(request.target());
        for (const auto& cookie : cookies_) {
            if (!cookie.configured_ &&
                config_.received_cookies_ == http_client_received_cookie_policy::ignore) {
                continue;
            }
            if (cookie.secure_ && config_.scheme_ != http_scheme::https) {
                continue;
            }
            // Domain matching is settled at storage: every stored domain is empty or
            // matched the same fixed origin host.
            if (!cookie_path_matches(path, cookie.path_)) {
                continue;
            }
            if (cookie_header_bytes != 0) {
                cookie_header_bytes += 2;
            }
            cookie_header_bytes += cookie.name_.size() + 1U + cookie.value_.size();
        }
    }
    std::string_view cookie_header_view{};
    if (cookie_header_bytes != 0) {
        if (cookie_header_bytes <= scratch.inline_cookie_header_.size()) {
            std::size_t cursor = 0;
            const auto append_inline = [&](std::string_view text) noexcept {
                if (!text.empty()) {
                    std::memcpy(scratch.inline_cookie_header_.data() + cursor, text.data(), text.size());
                    cursor += text.size();
                }
            };
            if (has_input_cookie) {
                for (const auto& field : input_headers) {
                    if (field.name().size() != 6U || !http_ascii_equals_ignore_case(field.name(), "cookie")) {
                        continue;
                    }
                    if (cursor != 0) {
                        append_inline("; ");
                    }
                    append_inline(field.value());
                }
            }
            if (check_jar) {
                for (const auto& cookie : cookies_) {
                    if (!cookie.configured_ &&
                        config_.received_cookies_ == http_client_received_cookie_policy::ignore) {
                        continue;
                    }
                    if (cookie.secure_ && config_.scheme_ != http_scheme::https) {
                        continue;
                    }
                    if (!cookie_path_matches(path, cookie.path_)) {
                        continue;
                    }
                    if (cursor != 0) {
                        append_inline("; ");
                    }
                    append_inline(cookie.name_);
                    scratch.inline_cookie_header_[cursor++] = '=';
                    append_inline(cookie.value_);
                }
            }
            cookie_header_view = std::string_view(scratch.inline_cookie_header_.data(), cursor);
        } else {
            auto& cookie_header = scratch.overflow_cookie_header_;
            cookie_header.reserve(cookie_header_bytes);
            if (has_input_cookie) {
                for (const auto& field : input_headers) {
                    if (field.name().size() != 6U || !http_ascii_equals_ignore_case(field.name(), "cookie")) {
                        continue;
                    }
                    if (!cookie_header.empty()) {
                        cookie_header.append("; ");
                    }
                    cookie_header.append(field.value());
                }
            }
            if (check_jar) {
                for (const auto& cookie : cookies_) {
                    if (!cookie.configured_ &&
                        config_.received_cookies_ == http_client_received_cookie_policy::ignore) {
                        continue;
                    }
                    if (cookie.secure_ && config_.scheme_ != http_scheme::https) {
                        continue;
                    }
                    if (!cookie_path_matches(path, cookie.path_)) {
                        continue;
                    }
                    append_cookie_request_pair(cookie_header, cookie.name_, cookie.value_);
                }
            }
            cookie_header_view = cookie_header;
        }
    }
    const bool add_cookie = !cookie_header_view.empty();
    field_count += static_cast<std::size_t>(add_user_agent) + static_cast<std::size_t>(add_cookie);
    std::span<const http_header_view> prepared_headers{};
    if (field_count <= scratch.inline_headers_.size()) {
        std::size_t index = 0;
        for (const auto& field : input_headers) {
            if (!has_input_cookie || !http_ascii_equals_ignore_case(field.name(), "cookie")) {
                scratch.inline_headers_[index++] = http_header_view{field.name(), field.value()};
            }
        }
        if (add_user_agent) {
            scratch.inline_headers_[index++] = http_header_view{"user-agent", config_.user_agent_};
        }
        if (add_cookie) {
            scratch.inline_headers_[index++] = http_header_view{"cookie", cookie_header_view};
        }
        prepared_headers = std::span<const http_header_view>(scratch.inline_headers_.data(), index);
    } else {
        auto& headers = scratch.overflow_headers_;
        headers.clear();
        headers.reserve(field_count);
        for (const auto& field : input_headers) {
            if (!has_input_cookie || !http_ascii_equals_ignore_case(field.name(), "cookie")) {
                headers.emplace_back(field.name(), field.value());
            }
        }
        if (add_user_agent) {
            headers.emplace_back("user-agent", config_.user_agent_);
        }
        if (add_cookie) {
            headers.emplace_back("cookie", cookie_header_view);
        }
        prepared_headers = std::span<const http_header_view>(headers);
    }
    http_client_request_view result;
    result.method_ = request.method();
    result.target_ = request.target();
    result.headers_ = prepared_headers;
    result.content_ = request.has_body() ? http_client_request_content_view::bytes(request.body())
                                         : http_client_request_content_view::none();
    return result;
}

void client_request_policy::retain_response_cookies(
    std::string_view request_target, std::span<const http_header> headers) {
    if (config_.received_cookies_ == http_client_received_cookie_policy::ignore) {
        return;
    }
    std::optional<std::chrono::system_clock::time_point> now;
    const auto current_time = [&now] {
        if (!now.has_value()) {
            now = std::chrono::system_clock::now();
        }
        return *now;
    };
    bool discarded_expired = cookies_.empty();
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "set-cookie")) {
            continue;
        }
        if (!discarded_expired) {
            discarded_expired = true;
            discard_expired(current_time());
        }
        const auto parsed_cookie = parse_set_cookie(header.value());
        if (!parsed_cookie) {
            continue;
        }
        if (const auto accepted = accept_set_cookie(*parsed_cookie, config_, request_target, current_time())) {
            store_received_cookie(*accepted);
        }
    }
}

void client_request_policy::store_received_cookie(const client_received_cookie& cookie) {
    // RFC 6265 section 5.3 step 11: name, domain and path identify a cookie;
    // a host-only cookie's domain is the request host.
    const auto identity_domain = [this](std::string_view domain) {
        return domain.empty() ? std::string_view(config_.host_) : domain;
    };
    const auto cookie_domain = identity_domain(cookie.domain_);
    const auto match = std::ranges::find_if(cookies_, [&](const stored_cookie& stored) {
        return !stored.configured_ && stored.name_ == cookie.name_ && stored.path_ == cookie.path_ &&
               http_ascii_equals_ignore_case(identity_domain(stored.domain_), cookie_domain);
    });
    const auto replaced_bytes =
        match == cookies_.end() ? 0 : storage_bytes(match->name_, match->value_, match->path_, match->domain_);
    if (cookie.expired_) {
        if (match != cookies_.end()) {
            cookie_bytes_ -= replaced_bytes;
            cookies_.erase(match);
        }
        return;
    }
    const auto replacement_bytes = storage_bytes(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
    if (!has_capacity(replaced_bytes, replacement_bytes, match == cookies_.end())) {
        return;
    }

    if (match == cookies_.end()) {
        stored_cookie replacement(
            cookie.name_, cookie.value_, cookie.path_, cookie.domain_, resource_);
        replacement.expires_ = cookie.expires_;
        replacement.secure_ = cookie.secure_;
        replacement.configured_ = false;
        // Longer paths first, so more specific cookies lead the Cookie header
        // (RFC 6265 section 5.4 step 2).
        const auto insertion = std::ranges::find_if(cookies_,
            [&cookie](const stored_cookie& stored) { return stored.path_.size() < cookie.path_.size(); });
        const auto insertion_index = insertion - cookies_.begin();
        reserve_cookie_slot();
        cookies_.emplace(cookies_.begin() + insertion_index, std::move(replacement));
    } else {
        std::optional<std::pmr::string> replacement_value;
        if (match->value_ != cookie.value_ && cookie.value_.size() > match->value_.capacity()) {
            replacement_value.emplace(cookie.value_, resource_);
        }
        std::optional<std::pmr::string> replacement_domain;
        if (match->domain_ != cookie.domain_ && cookie.domain_.size() > match->domain_.capacity()) {
            replacement_domain.emplace(cookie.domain_, resource_);
        }
        if (replacement_value.has_value()) {
            match->value_.swap(*replacement_value);
        } else if (match->value_ != cookie.value_) {
            match->value_.assign(cookie.value_);
        }
        if (replacement_domain.has_value()) {
            match->domain_.swap(*replacement_domain);
        } else if (match->domain_ != cookie.domain_) {
            match->domain_.assign(cookie.domain_);
        }
        match->expires_ = cookie.expires_;
        match->secure_ = cookie.secure_;
    }
    cookie_bytes_ = cookie_bytes_ - replaced_bytes + replacement_bytes;
}

}  // namespace ruvia::detail
