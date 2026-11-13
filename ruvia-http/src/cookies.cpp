#include "ruvia/http/cookies.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/ip_address.h"
#include "ruvia/http/proxy_protocol.h"
#include "ruvia/http/url_encoding.h"

#include "http_connection_fields.h"
#include "http_zlib_stream.h"
namespace ruvia::detail {

// RFC 6265bis section 5.6 storage limits. The sender rejects anything the
// receiving parser would truncate or drop, so set_cookie can find and replace
// cookies it emitted earlier by re-parsing them.
inline constexpr std::size_t max_cookie_name_value_bytes = 4096;
inline constexpr std::size_t max_cookie_attribute_value_bytes = 1024;

[[nodiscard]] inline bool is_valid_cookie_attribute(std::string_view value) noexcept {
    // RFC 6265bis path-value is *av-octet: ASCII %x20-3A / %x3C-7E.
    // HTTP field values can carry HTAB and obs-text, but emitting either inside
    // Path produces a Set-Cookie value outside the server grammar.
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte > 0x7e || c == ';') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool is_valid_cookie_domain(std::string_view value) noexcept {
    // RFC 6265bis domain-value is an RFC 1034 subdomain as relaxed by RFC 1123:
    // ASCII LDH labels, each at most 63 octets, with a letter or digit at both
    // ends. The root separators consume the remaining two octets of DNS's
    // 255-octet wire limit, so an unqualified textual name is at most 253.
    return value.empty() || is_valid_dns_domain_name(value);
}

[[nodiscard]] inline std::string_view cookie_priority_token(cookie_priority priority) noexcept {
    switch (priority) {
        case cookie_priority::low:
            return "Low";
        case cookie_priority::medium:
            return "Medium";
        case cookie_priority::high:
            return "High";
    }
    return {};
}

[[nodiscard]] inline std::string_view cookie_same_site_token(cookie_same_site same_site) noexcept {
    switch (same_site) {
        case cookie_same_site::strict:
            return "Strict";
        case cookie_same_site::lax:
            return "Lax";
        case cookie_same_site::none:
            return "None";
    }
    return {};
}

[[nodiscard]] inline bool cookie_attribute_emitted(cookie_attribute_policy policy) {
    switch (policy) {
        case cookie_attribute_policy::omit:
            return false;
        case cookie_attribute_policy::emit:
            return true;
    }
    throw std::invalid_argument("invalid cookie attribute policy");
}

// `secure` and `partitioned` are resolved attribute policies; the plan also
// checks Max-Age before converting it to unsigned storage.
inline void validate_cookie(std::string_view name, std::string_view value, const cookie_options& options,
    bool secure, bool partitioned) {
    if (!is_valid_http_header_name(name)) {
        throw std::invalid_argument("invalid cookie name");
    }
    if (!::ruvia::is_valid_cookie_value(value)) {
        throw std::invalid_argument("invalid cookie value");
    }
    const auto prefix_size = options.prefix_ ? ::ruvia::http_cookie_prefix_text(*options.prefix_).size() : 0;
    if (name.size() > max_cookie_name_value_bytes - prefix_size ||
        value.size() > max_cookie_name_value_bytes - prefix_size - name.size()) {
        throw std::invalid_argument("cookie name and value exceed 4096 bytes");
    }
    // Receivers trim OWS around Path, so edge spaces would not round-trip and
    // trailing SP would end the Set-Cookie field value in whitespace.
    const std::string_view path = options.path_;
    if (!is_valid_cookie_attribute(path) || path.size() > max_cookie_attribute_value_bytes ||
        http_trim_ows(path).size() != path.size()) {
        throw std::invalid_argument("invalid cookie attribute");
    }
    if (!is_valid_cookie_domain(options.domain_)) {
        throw std::invalid_argument("invalid cookie domain");
    }
    if (options.expires_.has_value()) {
        using namespace std::chrono;
        constexpr auto minimum = duration_cast<seconds>(sys_days{year{1601} / January / 1}.time_since_epoch());
        const auto expires = floor<seconds>(options.expires_->time_since_epoch());
        if (expires < minimum) {
            throw std::invalid_argument("cookie Expires must not precede year 1601");
        }
    }
    if (options.priority_ && cookie_priority_token(*options.priority_).empty()) {
        throw std::invalid_argument("invalid cookie Priority");
    }
    if (options.same_site_ && cookie_same_site_token(*options.same_site_).empty()) {
        throw std::invalid_argument("invalid cookie SameSite");
    }
    if (options.same_site_ == cookie_same_site::none && !secure) {
        throw std::invalid_argument("SameSite=None cookie requires Secure");
    }
    if (options.prefix_ && ::ruvia::http_cookie_prefix_text(*options.prefix_).empty()) {
        throw std::invalid_argument("invalid cookie prefix");
    }
    if (partitioned && !secure) {
        throw std::invalid_argument("partitioned cookie requires Secure");
    }
    // User agents apply __Host-/__Secure- constraints case-insensitively. Mirror
    // that receive-side rule for literal wire names so every cookie emitted by
    // this sender can actually be stored. A typed prefix is canonical and is the
    // outermost wire prefix, so it takes precedence over bytes in `name`.
    const auto prefix = options.prefix_ ? options.prefix_ : ::ruvia::cookie_name_prefix(name);
    if (prefix && !::ruvia::cookie_prefix_requirements_met(*prefix, secure, options.path_, options.domain_)) {
        throw std::invalid_argument(*prefix == cookie_prefix::host
                                        ? "__Host- cookie requires Secure, Path=/, and no Domain"
                                        : "__Secure- cookie requires Secure");
    }
}

}  // namespace ruvia::detail

namespace ruvia {

set_cookie_plan::set_cookie_plan(
    std::string_view name, std::string_view value, const cookie_options& options)
    : name_(name),
      value_(value),
      path_(options.path_),
      domain_(options.domain_),
      prefix_text_(options.prefix_ ? http_cookie_prefix_text(*options.prefix_) : std::string_view{}),
      priority_text_(options.priority_ ? detail::cookie_priority_token(*options.priority_) : std::string_view{}),
      same_site_text_(options.same_site_ ? detail::cookie_same_site_token(*options.same_site_) : std::string_view{}),
      http_only_(detail::cookie_attribute_emitted(options.http_only_)),
      secure_(detail::cookie_attribute_emitted(options.secure_)),
      partitioned_(detail::cookie_attribute_emitted(options.partitioned_)) {
    // max-age-av starts with a non-zero digit (RFC 6265 section 4.1.1);
    // an immediate expiry is written as the past deletion Expires instead.
    const bool expire_now = options.max_age_.has_value() && options.max_age_->count() == 0;
    if (expire_now || options.expires_.has_value()) {
        const auto expires = expire_now ? cookie_deletion_expires : *options.expires_;
        const auto seconds = std::chrono::floor<std::chrono::seconds>(expires.time_since_epoch()).count();
        if (!std::in_range<std::time_t>(seconds)) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        const auto date = format_http_date(static_cast<std::time_t>(seconds));
        if (!date) {
            throw std::invalid_argument("cookie Expires is not representable");
        }
        expires_size_ = date->size();
        std::memcpy(expires_buffer_.data(), date->data(), expires_size_);
    }
    if (options.max_age_.has_value() && options.max_age_->count() != 0) {
        if (options.max_age_->count() < 0) {
            throw std::invalid_argument("cookie Max-Age must not be negative");
        }
        max_age_value_ = static_cast<std::uint64_t>(options.max_age_->count());
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
    if (max_age_value_ != 0) {
        add_size(std::string_view("; Max-Age=").size());
        add_size(http_unsigned_decimal_size(max_age_value_));
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

    detail::validate_cookie(name, value, options, secure_, partitioned_);
}

char* write_cookie_request_pair(char* out, std::string_view name, std::string_view value) {
    const std::string_view written(out, cookie_request_pair_size(name, value));
    const bool name_overlaps = detail::http_text_overlaps(name, written);
    const bool value_overlaps = detail::http_text_overlaps(value, written);
    if (name_overlaps || value_overlaps) {
        // Snapshot only bytes that writing the pair could overwrite.
        const auto name_bytes = name_overlaps ? name.size() : 0;
        const auto value_bytes = value_overlaps ? value.size() : 0;
        std::array<char, 256> stack_snapshot;
        if (value_bytes <= stack_snapshot.size() &&
            name_bytes <= stack_snapshot.size() - value_bytes) {
            if (name_overlaps) {
                std::memcpy(stack_snapshot.data(), name.data(), name_bytes);
            }
            if (value_overlaps) {
                std::memcpy(stack_snapshot.data() + name_bytes, value.data(), value_bytes);
            }
            return write_cookie_request_pair(out,
                name_overlaps ? std::string_view(stack_snapshot.data(), name_bytes) : name,
                value_overlaps ? std::string_view(stack_snapshot.data() + name_bytes, value_bytes) : value);
        }
        std::string snapshot;
        snapshot.reserve(name_bytes + value_bytes);
        if (name_overlaps) {
            snapshot.append(name);
        }
        if (value_overlaps) {
            snapshot.append(value);
        }
        return write_cookie_request_pair(out,
            name_overlaps ? std::string_view(snapshot.data(), name_bytes) : name,
            value_overlaps ? std::string_view(snapshot.data() + name_bytes, value_bytes) : value);
    }
    detail::http_raw_wire_sink sink(out);
    sink.append(name);
    sink.append('=');
    sink.append(value);
    return sink.position();
}

void set_cookie_plan::write(char* cursor_value) const {
    (void)write_fields(cursor_value);
}

set_cookie_plan::written_fields set_cookie_plan::write_fields(char* cursor_value) const noexcept {
    written_fields fields;
    detail::http_raw_wire_sink sink(cursor_value);
    const auto* const name_begin = sink.position();
    sink.append(prefix_text_);
    sink.append(name_);
    fields.wire_name_ = {name_begin, static_cast<std::size_t>(sink.position() - name_begin)};
    sink.append('=');
    sink.append(value_);
    if (!path_.empty()) {
        sink.append("; Path=");
        fields.path_ = {sink.position(), path_.size()};
        sink.append(path_);
    }
    if (!domain_.empty()) {
        sink.append("; Domain=");
        fields.domain_ = {sink.position(), domain_.size()};
        sink.append(domain_);
    }
    if (max_age_value_ != 0) {
        // The constructor sized this slice with http_unsigned_decimal_size().
        sink.append("; Max-Age=");
        sink.append_unsigned(max_age_value_);
    }
    if (expires_size_ != 0) {
        sink.append("; Expires=");
        sink.append(std::string_view(expires_buffer_.data(), expires_size_));
    }
    if (http_only_) {
        sink.append("; HttpOnly");
    }
    if (secure_) {
        sink.append("; Secure");
    }
    if (!same_site_text_.empty()) {
        sink.append("; SameSite=");
        sink.append(same_site_text_);
    }
    if (!priority_text_.empty()) {
        sink.append("; Priority=");
        sink.append(priority_text_);
    }
    if (partitioned_) {
        sink.append("; Partitioned");
    }
    return fields;
}

}  // namespace ruvia

namespace ruvia {

bool is_valid_cookie_value(std::string_view value) noexcept {
    // cookie-value = *cookie-octet / ( DQUOTE *cookie-octet DQUOTE ); the quotes
    // are part of the value (RFC 6265 section 4.1.1).
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    for (const auto c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7f || c == '"' || c == ',' || c == ';' || c == '\\') {
            return false;
        }
    }
    return true;
}

std::optional<cookie_prefix> cookie_name_prefix(std::string_view name) noexcept {
    const auto starts_with_ignore_case = [name](std::string_view prefix) noexcept {
        return name.size() >= prefix.size() && http_ascii_equals_ignore_case(name.substr(0, prefix.size()), prefix);
    };
    if (starts_with_ignore_case("__Host-")) {
        return cookie_prefix::host;
    }
    if (starts_with_ignore_case("__Secure-")) {
        return cookie_prefix::secure;
    }
    return std::nullopt;
}

bool cookie_prefix_requirements_met(
    cookie_prefix prefix, bool secure, std::string_view path, std::string_view domain) noexcept {
    switch (prefix) {
        case cookie_prefix::secure:
            return secure;
        case cookie_prefix::host:
            return secure && path == "/" && domain.empty();
    }
    return false;
}

std::string_view http_cookie_prefix_text(cookie_prefix prefix) noexcept {
    switch (prefix) {
        case cookie_prefix::secure:
            return "__Secure-";
        case cookie_prefix::host:
            return "__Host-";
    }
    return {};
}

namespace {

[[nodiscard]] bool is_cookie_ip_host(std::string_view host) noexcept {
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
        return true;
    }
    return ip_address::parse(host).has_value();
}

[[nodiscard]] std::chrono::system_clock::time_point compute_cookie_expiration(
    std::chrono::system_clock::time_point now, std::int64_t max_age_seconds) noexcept {
    using clock_type = std::chrono::system_clock;
    const auto available =
        std::chrono::duration_cast<std::chrono::seconds>(clock_type::time_point::max() - now);
    if (std::chrono::seconds(max_age_seconds) >= available) {
        return clock_type::time_point::max();
    }
    return now +
           std::chrono::duration_cast<clock_type::duration>(std::chrono::seconds(max_age_seconds));
}

[[nodiscard]] bool satisfies_cookie_name_prefix(
    const http_set_cookie_view& cookie, bool secure) noexcept {
    const auto prefix = cookie_name_prefix(cookie.name());
    return !prefix ||
           cookie_prefix_requirements_met(*prefix, secure, cookie.path(), cookie.domain());
}

}  // namespace

http_cookie_jar::http_cookie_jar(const http_origin_view& origin, http_cookie_jar_options options)
    : http_cookie_jar(origin.scheme(), origin.host(), options) {}

http_cookie_jar::http_cookie_jar(
    http_scheme scheme, std::string_view host, http_cookie_jar_options options)
    : host_(host),
      scheme_(scheme),
      max_cookies_(options.max_cookies_),
      max_cookie_bytes_(options.max_cookie_bytes_) {
    if (!host_.empty() && host_.front() == '[' && host_.back() == ']') {
        host_.erase(host_.size() - 1, 1);
        host_.erase(0, 1);
    }
}

bool http_cookie_jar::cookie_domain_matches(
    std::string_view host, std::string_view domain) noexcept {
    if (domain.empty()) {
        return true;
    }
    if (http_ascii_equals_ignore_case(host, domain)) {
        return true;
    }
    if (is_cookie_ip_host(host)) {
        return false;
    }
    return host.size() > domain.size() && host[host.size() - domain.size() - 1] == '.' &&
           http_ascii_equals_ignore_case(host.substr(host.size() - domain.size()), domain);
}

bool http_cookie_jar::cookie_path_matches(
    std::string_view request_path, std::string_view cookie_path) noexcept {
    if (cookie_path.empty() || cookie_path == "/") {
        return !request_path.empty() && request_path.front() == '/';
    }
    if (!request_path.starts_with(cookie_path)) {
        return false;
    }
    return request_path.size() == cookie_path.size() || cookie_path.back() == '/' ||
           request_path[cookie_path.size()] == '/';
}

std::string_view http_cookie_jar::request_path_only(std::string_view target) noexcept {
    return target.substr(0, target.find_first_of("?#"));
}

std::string_view http_cookie_jar::default_cookie_path(std::string_view target) noexcept {
    const auto path = request_path_only(target);
    if (path.empty() || path.front() != '/') {
        return "/";
    }
    const auto slash = path.rfind('/');
    return slash == 0 || slash == std::string_view::npos ? std::string_view("/")
                                                         : path.substr(0, slash);
}

std::size_t http_cookie_jar::entry_storage_bytes(std::string_view name, std::string_view value,
    std::string_view path, std::string_view domain) noexcept {
    std::size_t total = 0;
    for (const auto field : {name, value, path, domain}) {
        if (field.size() > (std::numeric_limits<std::size_t>::max)() - total) {
            return (std::numeric_limits<std::size_t>::max)();
        }
        total += field.size();
    }
    return total;
}

bool http_cookie_jar::has_capacity(
    std::size_t replaced_bytes, std::size_t replacement_bytes, bool adding) const noexcept {
    if (adding && cookies_.size() >= max_cookies_) {
        return false;
    }
    const auto retained_bytes = cookie_bytes_ - replaced_bytes;
    return replacement_bytes <=
           max_cookie_bytes_ - std::min(retained_bytes, max_cookie_bytes_);
}

void http_cookie_jar::reserve_cookie_slot() {
    if (cookies_.size() < cookies_.capacity()) {
        return;
    }
    const auto remaining = max_cookies_ - cookies_.size();
    const auto growth = std::min(remaining, std::max(std::size_t{4}, cookies_.size()));
    cookies_.reserve(cookies_.size() + growth);
}

void http_cookie_jar::recompute_next_expiry() noexcept {
    next_expiry_.reset();
    for (const auto& cookie : cookies_) {
        if (cookie.expires_.has_value() &&
            (!next_expiry_.has_value() || *cookie.expires_ < *next_expiry_)) {
            next_expiry_ = *cookie.expires_;
        }
    }
}

void http_cookie_jar::discard_expired(std::chrono::system_clock::time_point now) {
    std::optional<std::chrono::system_clock::time_point> earliest_expiry;
    std::erase_if(cookies_, [this, now, &earliest_expiry](const stored_cookie& cookie) {
        if (!cookie.expires_.has_value()) {
            return false;
        }
        if (*cookie.expires_ > now) {
            if (!earliest_expiry.has_value() || *cookie.expires_ < *earliest_expiry) {
                earliest_expiry = *cookie.expires_;
            }
            return false;
        }
        cookie_bytes_ -=
            entry_storage_bytes(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
        return true;
    });
    next_expiry_ = earliest_expiry;
}

void http_cookie_jar::clear() noexcept {
    cookies_.clear();
    next_expiry_.reset();
    cookie_bytes_ = 0;
}

void http_cookie_jar::add_configured_cookie(std::string_view name, std::string_view value) {
    cookies_.emplace_back(name, value);
    cookie_bytes_ += entry_storage_bytes(name, value, "/", {});
}

std::optional<http_cookie_jar::received_cookie> http_cookie_jar::accept_set_cookie_view(
    std::string_view request_target, std::string_view set_cookie_value,
    std::chrono::system_clock::time_point now) const {
    const auto parsed = parse_set_cookie(set_cookie_value);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    const bool secure = parsed->has(http_set_cookie_attribute::secure);
    const bool partitioned = parsed->has(http_set_cookie_attribute::partitioned);
    if ((secure && scheme_ != http_scheme::https) ||
        !cookie_domain_matches(host_, parsed->domain()) ||
        (parsed->same_site_none() && !secure) ||
        (partitioned && !secure) ||
        !satisfies_cookie_name_prefix(*parsed, secure)) {
        return std::nullopt;
    }

    received_cookie cookie{
        .name_ = parsed->name(),
        .value_ = parsed->value(),
        .path_ = parsed->path().empty() ? default_cookie_path(request_target) : parsed->path(),
        .domain_ = parsed->domain(),
        .expires_ = std::nullopt,
        .secure_ = secure,
        .partitioned_ = partitioned,
    };
    if (const auto max_age_seconds = parsed->max_age_seconds()) {
        cookie.expired_ = *max_age_seconds <= 0;
        if (!cookie.expired_) {
            cookie.expires_ = compute_cookie_expiration(now, *max_age_seconds);
        }
    } else if (const auto expires_at = parsed->expires()) {
        cookie.expired_ = *expires_at <= std::chrono::system_clock::to_time_t(now);
        if (!cookie.expired_) {
            const auto expiration_limit = std::chrono::system_clock::to_time_t(
                compute_cookie_expiration(now, max_cookie_age_seconds));
            cookie.expires_ =
                std::chrono::system_clock::from_time_t(std::min(*expires_at, expiration_limit));
        }
    }
    return cookie;
}

bool http_cookie_jar::store_received_cookie(const received_cookie& cookie) {
    const auto identity_domain = [this](std::string_view domain) {
        return domain.empty() ? std::string_view(host_) : domain;
    };
    const auto cookie_domain = identity_domain(cookie.domain_);
    const auto match = std::ranges::find_if(cookies_, [&](const stored_cookie& stored) {
        return !stored.configured_ && stored.name_ == cookie.name_ &&
               stored.path_ == cookie.path_ && stored.partitioned_ == cookie.partitioned_ &&
               http_ascii_equals_ignore_case(identity_domain(stored.domain_), cookie_domain);
    });
    const auto replaced_bytes =
        match == cookies_.end()
            ? 0
            : entry_storage_bytes(match->name_, match->value_, match->path_, match->domain_);
    if (cookie.expired_) {
        if (match != cookies_.end()) {
            const bool removed_earliest =
                match->expires_.has_value() && next_expiry_ == match->expires_;
            cookie_bytes_ -= replaced_bytes;
            cookies_.erase(match);
            if (removed_earliest) {
                recompute_next_expiry();
            }
        }
        return true;
    }
    const auto replacement_bytes =
        entry_storage_bytes(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
    if (!has_capacity(replaced_bytes, replacement_bytes, match == cookies_.end())) {
        return false;
    }

    if (match == cookies_.end()) {
        stored_cookie replacement(cookie.name_, cookie.value_, cookie.path_, cookie.domain_);
        replacement.expires_ = cookie.expires_;
        replacement.secure_ = cookie.secure_;
        replacement.partitioned_ = cookie.partitioned_;
        replacement.configured_ = false;
        const auto insertion = std::ranges::find_if(cookies_,
            [&cookie](const stored_cookie& stored) { return stored.path_.size() < cookie.path_.size(); });
        const auto insertion_index = insertion - cookies_.begin();
        reserve_cookie_slot();
        cookies_.emplace(cookies_.begin() + insertion_index, std::move(replacement));
        if (cookie.expires_.has_value() &&
            (!next_expiry_.has_value() || *cookie.expires_ < *next_expiry_)) {
            next_expiry_ = *cookie.expires_;
        }
    } else {
        const auto old_expires = match->expires_;
        std::optional<std::string> replacement_value;
        if (match->value_ != cookie.value_ && cookie.value_.size() > match->value_.capacity()) {
            replacement_value.emplace(cookie.value_);
        }
        std::optional<std::string> replacement_domain;
        if (match->domain_ != cookie.domain_ && cookie.domain_.size() > match->domain_.capacity()) {
            replacement_domain.emplace(cookie.domain_);
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
        match->partitioned_ = cookie.partitioned_;
        if (cookie.expires_.has_value() &&
            (!next_expiry_.has_value() || *cookie.expires_ <= *next_expiry_)) {
            next_expiry_ = *cookie.expires_;
        } else if (old_expires.has_value() && next_expiry_ == old_expires) {
            recompute_next_expiry();
        }
    }
    cookie_bytes_ = cookie_bytes_ - replaced_bytes + replacement_bytes;
    return true;
}

bool http_cookie_jar::absorb_set_cookie(std::string_view request_target,
    std::string_view set_cookie_value, std::chrono::system_clock::time_point now) {
    if (!cookies_.empty() && next_expiry_.has_value() && now >= *next_expiry_) {
        discard_expired(now);
    }
    const auto accepted = accept_set_cookie_view(request_target, set_cookie_value, now);
    if (!accepted.has_value()) {
        return false;
    }
    return store_received_cookie(*accepted);
}

void http_cookie_jar::retain_response_cookies(std::string_view request_target,
    std::span<const http_header> headers, std::chrono::system_clock::time_point now) {
    bool discarded_expired = cookies_.empty();
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "set-cookie")) {
            continue;
        }
        if (!discarded_expired) {
            discarded_expired = true;
            if (next_expiry_.has_value() && now >= *next_expiry_) {
                discard_expired(now);
            }
        }
        if (const auto accepted = accept_set_cookie_view(request_target, header.value(), now)) {
            (void)store_received_cookie(*accepted);
        }
    }
}

void http_cookie_jar::retain_response_cookies(std::string_view request_target,
    std::span<const http_header_view> headers, std::chrono::system_clock::time_point now) {
    bool discarded_expired = cookies_.empty();
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "set-cookie")) {
            continue;
        }
        if (!discarded_expired) {
            discarded_expired = true;
            if (next_expiry_.has_value() && now >= *next_expiry_) {
                discard_expired(now);
            }
        }
        if (const auto accepted = accept_set_cookie_view(request_target, header.value(), now)) {
            (void)store_received_cookie(*accepted);
        }
    }
}

std::string http_cookie_jar::cookie_header(std::string_view request_target,
    bool include_received, std::chrono::system_clock::time_point now) {
    std::string output;
    visit_matching_cookies(request_target, include_received, now,
        [&output](std::string_view name, std::string_view value) {
            append_cookie_request_pair(output, name, value);
        });
    return output;
}

}  // namespace ruvia

namespace ruvia {
namespace {

std::pair<std::string_view, std::string_view> split_attribute(std::string_view value) noexcept {
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) {
        return {http_trim_ows(value), {}};
    }
    return {http_trim_ows(value.substr(0, equals)), http_trim_ows(value.substr(equals + 1))};
}

std::optional<std::int64_t> parse_max_age_seconds(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }

    std::int64_t seconds = 0;
    const auto [parsed_value, error] =
        std::from_chars(value.data(), value.data() + value.size(), seconds);
    // Even overflow must consume the complete attribute before saturation.
    if (parsed_value != value.data() + value.size()) {
        return std::nullopt;
    }
    if (error == std::errc::result_out_of_range) {
        return value.front() == '-' ? std::numeric_limits<std::int64_t>::min() : max_cookie_age_seconds;
    }
    if (error != std::errc{}) {
        return std::nullopt;
    }
    return seconds > max_cookie_age_seconds ? max_cookie_age_seconds : seconds;
}

bool contains_rejected_received_cookie_control(std::string_view value) noexcept {
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte <= 0x08 || (byte >= 0x0a && byte <= 0x1f) || byte == 0x7f) {
            return true;
        }
    }
    return false;
}

bool is_cookie_date_delimiter(unsigned char byte) noexcept {
    return byte == 0x09 || (byte >= 0x20 && byte <= 0x2f) || (byte >= 0x3b && byte <= 0x40) ||
           (byte >= 0x5b && byte <= 0x60) || (byte >= 0x7b && byte <= 0x7e);
}

// Consume only on success. Any remaining suffix starts with a non-digit,
// as required by RFC 6265 section 5.1.1 and verified erratum 4148.
std::optional<int> consume_cookie_date_digits(
    std::string_view& value, std::size_t minimum_digits, std::size_t maximum_digits) noexcept {
    std::size_t digits = 0;
    int result_value = 0;
    while (digits < value.size() && value[digits] >= '0' && value[digits] <= '9') {
        if (digits == maximum_digits) {
            return std::nullopt;
        }
        result_value = result_value * 10 + (value[digits] - '0');
        ++digits;
    }
    if (digits < minimum_digits) {
        return std::nullopt;
    }
    value.remove_prefix(digits);
    return result_value;
}

struct cookie_date_time final {
    int hour_;
    int minute_;
    int second_;
};

std::optional<cookie_date_time> parse_cookie_date_time(std::string_view value) noexcept {
    const auto hour = consume_cookie_date_digits(value, 1, 2);
    if (!hour || value.empty() || value.front() != ':') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    const auto minute = consume_cookie_date_digits(value, 1, 2);
    if (!minute || value.empty() || value.front() != ':') {
        return std::nullopt;
    }
    value.remove_prefix(1);
    const auto second = consume_cookie_date_digits(value, 1, 2);
    if (!second) {
        return std::nullopt;
    }
    return cookie_date_time{*hour, *minute, *second};
}

std::optional<int> parse_cookie_date_month(std::string_view value) noexcept {
    if (value.size() < 3) {
        return std::nullopt;
    }
    value = value.substr(0, 3);
    for (std::size_t index = 0; index < detail::http_month_names.size(); ++index) {
        if (http_ascii_equals_ignore_case(value, detail::http_month_names[index])) {
            return static_cast<int>(index + 1);
        }
    }
    return std::nullopt;
}

std::optional<std::time_t> parse_cookie_date(std::string_view value) noexcept {
    std::optional<cookie_date_time> time;
    std::optional<int> day;
    std::optional<int> month;
    std::optional<int> year;

    std::size_t cursor_value = 0;
    while (cursor_value < value.size()) {
        while (cursor_value < value.size() &&
               is_cookie_date_delimiter(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        const auto begin = cursor_value;
        while (cursor_value < value.size() &&
               !is_cookie_date_delimiter(static_cast<unsigned char>(value[cursor_value]))) {
            ++cursor_value;
        }
        if (begin == cursor_value) {
            continue;
        }
        auto token = value.substr(begin, cursor_value - begin);
        if (!time) {
            time = parse_cookie_date_time(token);
            if (time) {
                continue;
            }
        }
        if (!day) {
            day = consume_cookie_date_digits(token, 1, 2);
            if (day) {
                continue;
            }
        }
        if (!month) {
            month = parse_cookie_date_month(token);
            if (month) {
                continue;
            }
        }
        if (!year) {
            year = consume_cookie_date_digits(token, 2, 4);
        }
    }

    if (!time || !day || !month || !year) {
        return std::nullopt;
    }
    if (*year >= 70 && *year <= 99) {
        *year += 1900;
    } else if (*year <= 69) {
        *year += 2000;
    }
    if (*year < 1601 || *day < 1 || *day > 31 || time->hour_ > 23 || time->minute_ > 59 ||
        time->second_ > 59) {
        return std::nullopt;
    }
    return detail::http_civil_to_time_t(*year, *month, *day, time->hour_, time->minute_, time->second_);
}

}  // namespace

std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept {
    const auto first_end = value.find(';');
    const auto cookie_pair = value.substr(0, first_end);
    // RFC 6265 section 5.2 steps 2 and 5: a pair without '=' or with an empty
    // name makes the user agent ignore the whole set-cookie-string.
    const auto equals = cookie_pair.find('=');
    if (equals == std::string_view::npos) {
        return std::nullopt;
    }
    const auto name = http_trim_ows(cookie_pair.substr(0, equals));
    const auto cookie_value = http_trim_ows(cookie_pair.substr(equals + 1));
    if (name.empty() || name.size() > detail::max_cookie_name_value_bytes ||
        cookie_value.size() > detail::max_cookie_name_value_bytes - name.size() ||
        contains_rejected_received_cookie_control(name) ||
        contains_rejected_received_cookie_control(cookie_value)) {
        return std::nullopt;
    }

    http_set_cookie_view result(name, cookie_value);
    std::size_t offset = first_end == std::string_view::npos ? value.size() : first_end + 1;
    while (offset < value.size()) {
        const auto end = value.find(';', offset);
        const auto [attribute, argument] = split_attribute(value.substr(
            offset, end == std::string_view::npos ? value.size() - offset : end - offset));
        if (contains_rejected_received_cookie_control(attribute) ||
            contains_rejected_received_cookie_control(argument)) {
            return std::nullopt;
        }
        if (argument.size() <= detail::max_cookie_attribute_value_bytes) {
            switch (attribute.size()) {
                case 4U:
                    if (http_ascii_equals_ignore_case(attribute, "Path")) {
                        // RFC 6265 section 5.2.4: a value that is empty or does not start
                        // with '/' selects the default-path; the last Path attribute wins.
                        result.path_ = argument.empty() || argument.front() != '/' ? std::string_view{} : argument;
                    }
                    break;
                case 6U:
                    if (http_ascii_equals_ignore_case(attribute, "Domain")) {
                        // RFC 6265 section 5.2.3: an empty value ignores this cookie-av; a
                        // leading '.' is dropped, so "Domain=." yields a host-only cookie.
                        // Domain-match against the request host is the storage step's job.
                        if (!argument.empty()) {
                            result.domain_ = argument.front() == '.' ? argument.substr(1) : argument;
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "Secure")) {
                        result.set(http_set_cookie_attribute::secure);
                    }
                    break;
                case 7U:
                    if (http_ascii_equals_ignore_case(attribute, "Expires")) {
                        if (const auto expires = parse_cookie_date(argument)) {
                            result.expires_ = expires;
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "Max-Age")) {
                        if (const auto seconds = parse_max_age_seconds(argument)) {
                            result.max_age_seconds_ = seconds;
                        }
                    }
                    break;
                case 8U:
                    if (http_ascii_equals_ignore_case(attribute, "SameSite")) {
                        if (http_ascii_equals_ignore_case(argument, "Strict")) {
                            result.same_site_ = cookie_same_site::strict;
                        } else if (http_ascii_equals_ignore_case(argument, "Lax")) {
                            result.same_site_ = cookie_same_site::lax;
                        } else if (http_ascii_equals_ignore_case(argument, "None")) {
                            result.same_site_ = cookie_same_site::none;
                        } else {
                            result.same_site_.reset();
                        }
                    } else if (http_ascii_equals_ignore_case(attribute, "HttpOnly")) {
                        result.set(http_set_cookie_attribute::http_only);
                    }
                    break;
                case 11U:
                    if (http_ascii_equals_ignore_case(attribute, "Partitioned")) {
                        result.set(http_set_cookie_attribute::partitioned);
                    }
                    break;
                default:
                    break;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    }
    return result;
}

}  // namespace ruvia

namespace ruvia {
namespace {

[[nodiscard]] bool is_valid_cors_origin_value(std::string_view origin) noexcept {
    return origin == "null" || is_valid_http_serialized_origin(origin);
}

[[nodiscard]] std::string to_lower_copy(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) {
        ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
    }
    return result;
}

[[nodiscard]] std::string join_tokens(std::span<const std::string> items) {
    std::string output;
    if (items.empty()) {
        return output;
    }
    std::size_t total = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            total += 2U;
        }
        total += items[index].size();
    }
    output.reserve(total);
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            output.append(", ");
        }
        output.append(items[index]);
    }
    return output;
}

[[nodiscard]] bool vary_value_contains_token(
    std::string_view vary_value, std::string_view target_token) noexcept {
    bool found = false;
    detail::http_visit_comma_separated_tokens(vary_value, [&](std::string_view token) noexcept {
        if (token == "*" || http_ascii_equals_ignore_case(token, target_token)) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

[[nodiscard]] bool headers_have_vary_token(
    std::span<const http_header> headers, std::string_view token) noexcept {
    for (const auto& header : headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Vary") &&
            vary_value_contains_token(header.value(), token)) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool is_http_cors_controlled_response_header(std::string_view name) noexcept {
    switch (name.size()) {
        case 22U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Max-Age");
        case 27U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Origin");
        case 28U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Methods") ||
                   http_ascii_equals_ignore_case(name, "Access-Control-Allow-Headers");
        case 29U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Expose-Headers");
        case 32U:
            return http_ascii_equals_ignore_case(name, "Access-Control-Allow-Credentials");
        default:
            return false;
    }
}

void merge_http_vary_tokens(
    std::vector<http_header>& headers, std::span<const std::string_view> tokens) {
    std::vector<std::string_view> missing;
    for (const auto token : tokens) {
        const auto trimmed = http_trim_ows(token);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed == "*") {
            if (!headers_have_vary_token(headers, "*")) {
                std::erase_if(headers, [](const http_header& field) noexcept {
                    return http_ascii_equals_ignore_case(field.name(), "Vary");
                });
                headers.emplace_back("Vary", "*");
            }
            return;
        }
        if (!is_valid_http_header_name(trimmed)) {
            throw std::invalid_argument("Vary token must be a valid HTTP field name");
        }
        if (headers_have_vary_token(headers, trimmed)) {
            continue;
        }
        const bool duplicate = std::ranges::any_of(missing, [trimmed](std::string_view current) noexcept {
            return http_ascii_equals_ignore_case(current, trimmed);
        });
        if (!duplicate) {
            missing.push_back(trimmed);
        }
    }
    if (missing.empty()) {
        return;
    }

    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (!http_ascii_equals_ignore_case(headers[index].name(), "Vary")) {
            continue;
        }
        std::string combined;
        for (const auto& field : headers) {
            if (!http_ascii_equals_ignore_case(field.name(), "Vary")) {
                continue;
            }
            const auto trimmed = http_trim_ows(field.value());
            if (trimmed.empty()) {
                continue;
            }
            if (!combined.empty()) {
                combined.append(", ");
            }
            combined.append(trimmed);
        }
        for (const auto token : missing) {
            if (!combined.empty()) {
                combined.append(", ");
            }
            combined.append(token);
        }
        headers[index] = http_header("Vary", combined);
        for (std::size_t rest = headers.size(); rest > index + 1; --rest) {
            if (http_ascii_equals_ignore_case(headers[rest - 1].name(), "Vary")) {
                headers.erase(headers.begin() + static_cast<std::ptrdiff_t>(rest - 1));
            }
        }
        return;
    }

    std::string value;
    for (std::size_t index = 0; index < missing.size(); ++index) {
        if (index != 0) {
            value.append(", ");
        }
        value.append(missing[index]);
    }
    headers.emplace_back("Vary", value);
}

void merge_http_vary_token(std::vector<http_header>& headers, std::string_view token) {
    merge_http_vary_tokens(headers, std::span<const std::string_view>(&token, 1));
}

http_cors_policy::http_cors_policy(const http_cors_policy_config& config)
    : allow_any_origin_(config.allow_any_origin_),
      reflect_request_headers_(config.reflect_request_headers_),
      allow_credentials_(config.allow_credentials_) {
    const auto add_suffix = [this](std::string_view pattern) {
        if (pattern.empty()) {
            throw std::invalid_argument("CORS origin suffix must not be empty");
        }
        suffix_rule rule;
        auto rest = pattern;
        if (const auto scheme_pos = rest.find("://"); scheme_pos != std::string_view::npos) {
            const auto scheme = rest.substr(0, scheme_pos);
            if (!http_ascii_equals_ignore_case(scheme, "http") &&
                !http_ascii_equals_ignore_case(scheme, "https")) {
                throw std::invalid_argument("CORS origin suffix scheme must be http or https");
            }
            rule.scheme_ = to_lower_copy(scheme);
            rest = rest.substr(scheme_pos + 3U);
        }
        if (rest.starts_with("*.")) {
            rule.subdomain_only_ = true;
            rest.remove_prefix(2U);
        } else if (rest.starts_with('.')) {
            rule.subdomain_only_ = true;
            rest.remove_prefix(1U);
        }
        const auto authority = parse_http_authority(rest);
        if (!authority || authority->host().empty() ||
            authority->port_kind() == http_authority_port_kind::empty ||
            authority->host().starts_with('[') ||
            is_valid_http_ipv4_literal(authority->host()) ||
            authority->host().front() == '.' || authority->host().back() == '.') {
            throw std::invalid_argument("CORS origin suffix must name a valid domain");
        }
        rule.host_suffix_ = to_lower_copy(authority->host());
        rule.port_ = authority->port();
        suffix_rules_.push_back(std::move(rule));
    };

    for (const auto& origin_entry : config.allowed_origins_) {
        const std::string_view view(origin_entry);
        if (view == "*") {
            allow_any_origin_ = true;
            continue;
        }
        if (view.starts_with("*.") || view.starts_with('.') ||
            view.find("://*.") != std::string_view::npos ||
            view.find("://.") != std::string_view::npos) {
            add_suffix(view);
            continue;
        }
        if (!is_valid_cors_origin_value(view)) {
            throw std::invalid_argument("CORS allowed origin must be a WHATWG serialized origin");
        }
        exact_origins_.emplace_back(view);
    }

    for (const auto& suffix_entry : config.allowed_origin_suffixes_) {
        add_suffix(suffix_entry);
    }
    if (exact_origins_.empty() && suffix_rules_.empty()) {
        allow_any_origin_ = true;
    }
    for (const auto& method : config.allowed_methods_) {
        const std::string_view view(method);
        if (view == "*") {
            allow_any_method_ = true;
            continue;
        }
        if (!is_valid_http_method_token(view)) {
            throw std::invalid_argument("CORS allowed method must be a valid HTTP method token");
        }
        allowed_methods_.emplace_back(view);
    }
    if (allowed_methods_.empty()) {
        allow_any_method_ = true;
    }
    joined_allowed_methods_ = join_tokens(allowed_methods_);

    for (const auto& header_name : config.allowed_headers_) {
        const std::string_view view(header_name);
        if (view == "*") {
            reflect_request_headers_ = true;
            continue;
        }
        if (!is_valid_http_header_name(view)) {
            throw std::invalid_argument("CORS allowed header must be a valid HTTP field name");
        }
        allowed_headers_.emplace_back(view);
    }
    if (allowed_headers_.empty()) {
        reflect_request_headers_ = true;
    }
    joined_allowed_headers_ = join_tokens(allowed_headers_);
    for (const auto& expose_name : config.expose_headers_) {
        if (!is_valid_http_header_name(expose_name)) {
            throw std::invalid_argument("CORS exposed header must be a valid HTTP field name");
        }
    }
    joined_expose_headers_ = join_tokens(config.expose_headers_);

    if (config.max_age_.has_value()) {
        if (config.max_age_->count() < 0) {
            throw std::invalid_argument("CORS max age must not be negative");
        }
        std::array<char, http_max_unsigned_decimal_size> buffer{};
        const auto formatted =
            http_format_unsigned(buffer, static_cast<std::uint64_t>(config.max_age_->count()));
        max_age_.assign(formatted);
    }
}

bool http_cors_policy::allows_origin(std::string_view origin) const noexcept {
    if (!is_valid_cors_origin_value(origin)) {
        return false;
    }
    if (allow_any_origin_) {
        return true;
    }
    if (origin == "null") {
        return std::ranges::any_of(exact_origins_, [](std::string_view configured) noexcept {
            return configured == "null";
        });
    }

    const auto scheme_sep = origin.find("://");
    if (scheme_sep == std::string_view::npos) {
        return false;
    }
    const auto scheme = origin.substr(0, scheme_sep);
    const auto authority_text = origin.substr(scheme_sep + 3U);

    for (const auto& configured : exact_origins_) {
        if (configured == "null" || configured.size() != origin.size()) {
            continue;
        }
        const auto conf_sep = configured.find("://");
        if (conf_sep != scheme_sep ||
            !http_ascii_equals_ignore_case(scheme, std::string_view(configured).substr(0, conf_sep))) {
            continue;
        }
        if (http_ascii_equals_ignore_case(
                authority_text, std::string_view(configured).substr(conf_sep + 3U))) {
            return true;
        }
    }

    if (suffix_rules_.empty()) {
        return false;
    }
    const auto authority = parse_http_authority(authority_text);
    if (!authority || authority->host().empty() || authority->host().starts_with('[') ||
        is_valid_http_ipv4_literal(authority->host())) {
        return false;
    }
    const auto host = authority->host();
    const auto port = authority->port();

    for (const auto& rule : suffix_rules_) {
        if (!rule.scheme_.empty() && !http_ascii_equals_ignore_case(scheme, rule.scheme_)) {
            continue;
        }
        if (rule.port_.has_value() && port != rule.port_) {
            continue;
        }
        const std::string_view suffix(rule.host_suffix_);
        if (!rule.subdomain_only_ && http_ascii_equals_ignore_case(host, suffix)) {
            return true;
        }
        if (host.size() > suffix.size() && host[host.size() - suffix.size() - 1U] == '.' &&
            http_ascii_equals_ignore_case(host.substr(host.size() - suffix.size()), suffix)) {
            return true;
        }
    }
    return false;
}

bool http_cors_policy::allows_method(std::string_view method) const noexcept {
    if (!is_valid_http_method_token(method)) {
        return false;
    }
    if (allow_any_method_) {
        return true;
    }
    return std::ranges::any_of(allowed_methods_, [method](std::string_view allowed) noexcept {
        return http_ascii_equals_ignore_case(allowed, method);
    });
}

bool http_cors_policy::allows_request_header(std::string_view name) const noexcept {
    if (!is_valid_http_header_name(name)) {
        return false;
    }
    if (reflect_request_headers_) {
        return true;
    }
    return std::ranges::any_of(allowed_headers_, [name](std::string_view allowed) noexcept {
        return http_ascii_equals_ignore_case(allowed, name);
    });
}

http_cors_preflight_result http_cors_policy::evaluate_preflight(
    http_cors_preflight_input input) const {
    const auto method = input.method_.view();
    const auto origin = input.origin_.view();
    const auto requested_method = http_trim_ows(input.access_control_request_method_.view());
    const auto requested_headers = input.access_control_request_headers_.view();

    if (method != "OPTIONS" || origin.empty() || !allows_origin(origin) ||
        requested_method.empty() || !allows_method(requested_method)) {
        return http_cors_preflight_result();
    }

    std::string reflected_headers;
    if (!http_trim_ows(requested_headers).empty()) {
        http_field_name_list names(requested_headers);
        bool saw_name = false;
        while (const auto header_name = names.next()) {
            saw_name = true;
            if (!allows_request_header(*header_name)) {
                return http_cors_preflight_result();
            }
            if (reflect_request_headers_) {
                if (!reflected_headers.empty()) {
                    reflected_headers.append(", ");
                }
                reflected_headers.append(*header_name);
            }
        }
        if (!saw_name || !names.valid()) {
            return http_cors_preflight_result();
        }
    }

    std::vector<http_header> headers;
    headers.reserve(6);
    const std::string_view allow_origin =
        (allow_any_origin_ && !allow_credentials_) ? std::string_view("*") : origin;
    headers.emplace_back("Access-Control-Allow-Origin", allow_origin);
    if (allow_credentials_) {
        headers.emplace_back("Access-Control-Allow-Credentials", "true");
    }
    const std::string_view allow_methods =
        !joined_allowed_methods_.empty() ? std::string_view(joined_allowed_methods_) : requested_method;
    headers.emplace_back("Access-Control-Allow-Methods", allow_methods);
    if (reflect_request_headers_) {
        if (!reflected_headers.empty()) {
            headers.emplace_back("Access-Control-Allow-Headers", reflected_headers);
        }
    } else if (!joined_allowed_headers_.empty()) {
        headers.emplace_back("Access-Control-Allow-Headers", joined_allowed_headers_);
    }
    if (!max_age_.empty()) {
        headers.emplace_back("Access-Control-Max-Age", max_age_);
    }
    headers.emplace_back("Vary",
        reflect_request_headers_
            ? "Origin, Access-Control-Request-Method, Access-Control-Request-Headers"
            : "Origin, Access-Control-Request-Method");

    return http_cors_preflight_result(true, std::move(headers));
}

http_cors_preflight_result http_cors_policy::evaluate_preflight(
    const http_request& request) const {
    const auto origin = request.header("Origin");
    const auto requested_method = request.header("Access-Control-Request-Method");
    if (!origin.has_value() || !requested_method.has_value()) {
        return http_cors_preflight_result();
    }
    std::string combined_headers;
    for (const auto& field : request.headers()) {
        if (!http_ascii_equals_ignore_case(field.name(), "Access-Control-Request-Headers")) {
            continue;
        }
        if (http_trim_ows(field.value()).empty()) {
            return http_cors_preflight_result();
        }
        if (!combined_headers.empty()) {
            combined_headers.append(", ");
        }
        combined_headers.append(field.value());
    }
    return evaluate_preflight({
        .method_ = request.method(),
        .origin_ = *origin,
        .access_control_request_method_ = *requested_method,
        .access_control_request_headers_ = combined_headers,
    });
}

bool http_cors_policy::rewrite_response_headers(
    borrowed_text request_origin, std::vector<http_header>& response_headers) const {
    std::erase_if(response_headers, [](const http_header& field) noexcept {
        return is_http_cors_controlled_response_header(field.name());
    });

    const auto origin = request_origin.view();
    const bool granted = !origin.empty() && allows_origin(origin);
    if (!allow_any_origin_ || allow_credentials_) {
        merge_http_vary_token(response_headers, "Origin");
    }
    if (!granted) {
        return false;
    }

    const std::string_view allow_origin =
        (allow_any_origin_ && !allow_credentials_) ? std::string_view("*") : origin;
    response_headers.emplace_back("Access-Control-Allow-Origin", allow_origin);
    if (allow_credentials_) {
        response_headers.emplace_back("Access-Control-Allow-Credentials", "true");
    }
    if (!joined_expose_headers_.empty()) {
        response_headers.emplace_back("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

bool http_cors_policy::rewrite_response_headers(
    borrowed_text request_origin, http_response_headers& response_headers) const {
    response_headers.remove_header("Access-Control-Allow-Origin");
    response_headers.remove_header("Access-Control-Allow-Credentials");
    response_headers.remove_header("Access-Control-Expose-Headers");
    response_headers.remove_header("Access-Control-Max-Age");
    response_headers.remove_header("Access-Control-Allow-Methods");
    response_headers.remove_header("Access-Control-Allow-Headers");

    const auto origin = request_origin.view();
    const bool granted = !origin.empty() && allows_origin(origin);
    if (!allow_any_origin_ || allow_credentials_) {
        response_headers.add_vary_token("Origin");
    }
    if (!granted) {
        return false;
    }

    const std::string_view allow_origin =
        (allow_any_origin_ && !allow_credentials_) ? std::string_view("*") : origin;
    response_headers.header("Access-Control-Allow-Origin", allow_origin);
    if (allow_credentials_) {
        response_headers.header_stable_view("Access-Control-Allow-Credentials", "true");
    }
    if (!joined_expose_headers_.empty()) {
        response_headers.header("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

bool http_cors_policy::rewrite_response_headers(
    borrowed_text request_origin, http_response& response) const {
    auto staged = response.headers().clone(4);
    const bool granted = rewrite_response_headers(request_origin, staged);
    response.commit_headers(std::move(staged));
    return granted;
}

bool http_cors_policy::apply_response_headers(
    const http_request& request, http_response& response) const {
    const bool options = request.known_method() == http_known_method::options;
    if (options) {
        response.add_vary_token("Origin");
        response.add_vary_token("Access-Control-Request-Method");
        if (reflect_request_headers_) {
            response.add_vary_token("Access-Control-Request-Headers");
        }
    } else if (!allow_any_origin_ || allow_credentials_) {
        response.add_vary_token("Origin");
    }

    const auto origin = request.header("Origin");
    if (origin.has_value() && !allows_origin(*origin)) {
        return false;
    }
    const auto requested_method =
        options ? request.header("Access-Control-Request-Method") : std::nullopt;
    if (requested_method.has_value() && !allows_method(http_trim_ows(*requested_method))) {
        return false;
    }
    const bool preflight = options && origin.has_value() && requested_method.has_value();
    const bool reflect_headers =
        preflight && reflect_request_headers_ &&
        !response.header("Access-Control-Allow-Headers").has_value();
    std::string reflected_headers;
    if (options && request.header("Access-Control-Request-Headers").has_value()) {
        std::size_t request_header_names_size = 0;
        for (const auto& field : request.headers()) {
            if (field.name().size() != 30U ||
                !http_ascii_equals_ignore_case(field.name(), "Access-Control-Request-Headers")) {
                continue;
            }
            http_field_name_list names(field.value());
            bool saw_name = false;
            while (const auto header_name = names.next()) {
                saw_name = true;
                if (!allows_request_header(*header_name)) {
                    return false;
                }
                const auto separator_size =
                    request_header_names_size == 0 ? std::size_t{0} : std::size_t{2};
                const auto maximum = (std::numeric_limits<std::size_t>::max)();
                if (header_name->size() > maximum - separator_size ||
                    request_header_names_size > maximum - separator_size - header_name->size()) {
                    return false;
                }
                request_header_names_size += separator_size + header_name->size();
                if (reflect_headers) {
                    if (!reflected_headers.empty()) {
                        reflected_headers.append(", ");
                    }
                    reflected_headers.append(*header_name);
                }
            }
            if (!saw_name || !names.valid()) {
                return false;
            }
        }
    }

    if (!response.header("Access-Control-Allow-Origin").has_value()) {
        if (allow_any_origin_ && !allow_credentials_) {
            response.header_stable_view("Access-Control-Allow-Origin", "*");
        } else if (origin.has_value()) {
            response.header("Access-Control-Allow-Origin", *origin);
        } else if (exact_origins_.size() == 1 && suffix_rules_.empty()) {
            response.header("Access-Control-Allow-Origin", exact_origins_.front());
        }
    }
    if (allow_credentials_ && !response.header("Access-Control-Allow-Credentials").has_value()) {
        response.header_stable_view("Access-Control-Allow-Credentials", "true");
    }

    if (preflight) {
        if (!response.header("Access-Control-Allow-Methods").has_value()) {
            if (!joined_allowed_methods_.empty()) {
                response.header("Access-Control-Allow-Methods", joined_allowed_methods_);
            } else if (const auto allow = response.header("Allow");
                allow.has_value() && !allow->empty()) {
                response.header("Access-Control-Allow-Methods", *allow);
            } else {
                response.header("Access-Control-Allow-Methods", http_trim_ows(*requested_method));
            }
        }
        if (!response.header("Access-Control-Allow-Headers").has_value()) {
            if (!reflect_request_headers_ && !joined_allowed_headers_.empty()) {
                response.header("Access-Control-Allow-Headers", joined_allowed_headers_);
            } else if (reflect_request_headers_ && !reflected_headers.empty()) {
                response.header("Access-Control-Allow-Headers", reflected_headers);
            }
        }
        if (!max_age_.empty() && !response.header("Access-Control-Max-Age").has_value()) {
            response.header("Access-Control-Max-Age", max_age_);
        }
        return true;
    }

    if (!joined_expose_headers_.empty() &&
        !response.header("Access-Control-Expose-Headers").has_value()) {
        response.header("Access-Control-Expose-Headers", joined_expose_headers_);
    }
    return true;
}

}  // namespace ruvia

namespace ruvia {
namespace {

// "[2001:db8::1]:443" -> "2001:db8::1"; "192.0.2.1:8080" -> "192.0.2.1".
[[nodiscard]] std::string_view extract_forwarded_node_address(std::string_view node_value) noexcept {
    node_value = http_trim_quoted_field_value(http_trim_ows(node_value));
    if (!node_value.empty() && node_value.front() == '[') {
        const auto close = node_value.find(']');
        if (close == std::string_view::npos) {
            return {};
        }
        const auto rest = node_value.substr(close + 1);
        if (!rest.empty() && rest.front() != ':') {
            return {};
        }
        return node_value.substr(1, close - 1);
    }
    const auto colon = node_value.find(':');
    if (colon != std::string_view::npos &&
        node_value.find(':', colon + 1) == std::string_view::npos) {
        return node_value.substr(0, colon);
    }
    return node_value;
}

[[nodiscard]] std::optional<http_scheme> parse_forwarded_http_scheme(
    std::string_view token) noexcept {
    token = http_trim_quoted_field_value(http_trim_ows(token));
    if (http_ascii_equals_ignore_case(token, "http")) {
        return http_scheme::http;
    }
    if (http_ascii_equals_ignore_case(token, "https")) {
        return http_scheme::https;
    }
    return std::nullopt;
}

struct forwarded_hop final {
    std::optional<ip_address> address_{};
    std::optional<http_scheme> scheme_{};
};

[[nodiscard]] std::optional<ip_address> parse_hop_address(std::string_view node_value) noexcept {
    const auto address_text = extract_forwarded_node_address(node_value);
    if (address_text.empty()) {
        return std::nullopt;
    }
    const auto parsed = ip_address::parse(address_text);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return parsed->normalized_ipv4();
}

[[nodiscard]] forwarded_hop parse_rfc7239_element(std::string_view element) noexcept {
    forwarded_hop hop{};
    bool saw_for = false;
    bool saw_proto = false;
    http_visit_semicolon_parameters_quoted_field(
        element, [&](std::string_view name, std::string_view value) noexcept {
            if (http_ascii_equals_ignore_case(name, "for")) {
                if (saw_for) {
                    hop.address_.reset();
                } else {
                    saw_for = true;
                    hop.address_ = parse_hop_address(value);
                }
            } else if (http_ascii_equals_ignore_case(name, "proto")) {
                if (saw_proto) {
                    hop.scheme_.reset();
                } else {
                    saw_proto = true;
                    hop.scheme_ = parse_forwarded_http_scheme(value);
                }
            }
            return true;
        });
    return hop;
}

class forwarding_chain final {
public:
    void append(const http_client_ip_extractor& extractor, forwarded_hop hop) noexcept {
        if (size_ == 0) {
            leftmost_ = hop;
        }
        const bool trusted = hop.address_.has_value() && extractor.is_trusted_proxy(*hop.address_);
        if (trusted) {
            if (awaiting_nearest_trusted_) {
                nearest_trusted_ = hop.address_;
                awaiting_nearest_trusted_ = false;
            }
        } else {
            client_hop_ = hop;
            client_index_ = size_;
            client_identified_ = hop.address_.has_value();
            has_client_hop_ = true;
            nearest_trusted_.reset();
            awaiting_nearest_trusted_ = true;
        }
        ++size_;
    }

    [[nodiscard]] forwarded_hop client() const noexcept {
        if (!has_client_hop_) {
            return leftmost_;
        }
        return {client_identified_ ? client_hop_.address_ : nearest_trusted_, client_hop_.scheme_};
    }

    [[nodiscard]] std::size_t client_index() const noexcept {
        return has_client_hop_ ? client_index_ : 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

private:
    forwarded_hop leftmost_{};
    forwarded_hop client_hop_{};
    std::optional<ip_address> nearest_trusted_{};
    std::size_t client_index_{0};
    std::size_t size_{0};
    bool has_client_hop_{false};
    bool client_identified_{false};
    bool awaiting_nearest_trusted_{false};
};

template <typename header_span_type>
[[nodiscard]] std::optional<http_client_ip_result> resolve_from_header(
    const http_client_ip_extractor& extractor, const ip_address& normalized_peer,
    std::string_view header_name, http_forwarded_header_kind kind,
    header_span_type headers) noexcept {
    if (kind == http_forwarded_header_kind::single_ip) {
        std::string_view single_value{};
        std::size_t line_count = 0;
        for (const auto& field : headers) {
            if (http_ascii_equals_ignore_case(field.name(), header_name)) {
                single_value = field.value();
                ++line_count;
            }
        }
        if (line_count != 1) {
            return std::nullopt;
        }
        const auto trimmed = http_trim_ows(single_value);
        if (trimmed.empty() || trimmed.find(',') != std::string_view::npos) {
            return std::nullopt;
        }
        const auto parsed = parse_hop_address(trimmed);
        if (!parsed.has_value() || extractor.is_trusted_proxy(*parsed)) {
            return std::nullopt;
        }
        return http_client_ip_result{
            .address_ = *parsed, .from_forwarded_header_ = true, .scheme_ = std::nullopt};
    }

    forwarding_chain chain;
    for (const auto& field : headers) {
        if (!http_ascii_equals_ignore_case(field.name(), header_name)) {
            continue;
        }
        http_visit_comma_separated_quoted_field_items(
            field.value(), [&](std::string_view item) noexcept {
                if (item.empty()) {
                    return true;
                }
                if (kind == http_forwarded_header_kind::rfc7239_forwarded) {
                    chain.append(extractor, parse_rfc7239_element(item));
                } else {
                    chain.append(extractor, {.address_ = parse_hop_address(item), .scheme_ = std::nullopt});
                }
                return true;
            });
    }
    if (chain.size() == 0) {
        return std::nullopt;
    }
    auto resolved = chain.client();
    if (kind == http_forwarded_header_kind::comma_separated_list &&
        extractor.trust_x_forwarded_proto() &&
        http_ascii_equals_ignore_case(header_name, "X-Forwarded-For")) {
        std::size_t proto_count = 0;
        std::optional<http_scheme> matched_scheme{};
        const auto target_index = chain.client_index();
        for (const auto& field : headers) {
            if (!http_ascii_equals_ignore_case(field.name(), "X-Forwarded-Proto")) {
                continue;
            }
            http_visit_comma_separated_quoted_field_items(
                field.value(), [&](std::string_view item) noexcept {
                    if (item.empty()) {
                        return true;
                    }
                    if (proto_count == target_index) {
                        matched_scheme = parse_forwarded_http_scheme(item);
                    }
                    ++proto_count;
                    return true;
                });
        }
        if (proto_count == chain.size()) {
            resolved.scheme_ = matched_scheme;
        }
    }
    if (!resolved.address_.has_value() && !resolved.scheme_.has_value()) {
        return std::nullopt;
    }
    return http_client_ip_result{
        .address_ = resolved.address_.value_or(normalized_peer),
        .from_forwarded_header_ = true,
        .scheme_ = resolved.scheme_,
    };
}

[[nodiscard]] http_forwarded_header_kind resolve_header_kind(
    std::string_view name, http_forwarded_header_kind configured) noexcept {
    if (configured != http_forwarded_header_kind::auto_detect) {
        return configured;
    }
    if (http_ascii_equals_ignore_case(name, "Forwarded")) {
        return http_forwarded_header_kind::rfc7239_forwarded;
    }
    if (http_ascii_equals_ignore_case(name, "X-Forwarded-For")) {
        return http_forwarded_header_kind::comma_separated_list;
    }
    return http_forwarded_header_kind::single_ip;
}
}  // namespace

http_client_ip_extractor::http_client_ip_extractor(const http_client_ip_config& config)
    : enabled_(config.enabled_),
      trust_x_forwarded_proto_(config.trust_x_forwarded_proto_) {
    trusted_cidrs_.reserve(config.trusted_proxies_.size());
    for (const auto& cidr_text : config.trusted_proxies_) {
        const auto parsed = ip_cidr::parse(cidr_text);
        if (!parsed.has_value()) {
            throw std::invalid_argument("trusted proxy entry must be a valid IP address or CIDR");
        }
        trusted_cidrs_.push_back(*parsed);
    }

    if (config.forwarding_headers_.empty()) {
        compiled_header xff;
        xff.name_.assign("X-Forwarded-For");
        xff.kind_ = http_forwarded_header_kind::comma_separated_list;
        headers_.push_back(std::move(xff));

        compiled_header fwd;
        fwd.name_.assign("Forwarded");
        fwd.kind_ = http_forwarded_header_kind::rfc7239_forwarded;
        headers_.push_back(std::move(fwd));
        return;
    }

    headers_.reserve(config.forwarding_headers_.size());
    for (const auto& spec : config.forwarding_headers_) {
        if (!is_valid_http_header_name(spec.name_)) {
            throw std::invalid_argument("forwarding header name must be a valid HTTP field name");
        }
        compiled_header entry;
        entry.name_.assign(spec.name_);
        entry.kind_ = resolve_header_kind(spec.name_, spec.kind_);
        headers_.push_back(std::move(entry));
    }
}

bool http_client_ip_extractor::is_trusted_proxy(const ip_address& address) const noexcept {
    return std::ranges::any_of(trusted_cidrs_, [&address](const ip_cidr& cidr) noexcept {
        return cidr.contains(address);
    });
}

bool http_client_ip_extractor::is_trusted_proxy(std::string_view address_text) const noexcept {
    const auto parsed = ip_address::parse(address_text);
    return parsed.has_value() && is_trusted_proxy(*parsed);
}

http_client_ip_result http_client_ip_extractor::resolve(
    const ip_address& peer_ip, std::span<const http_header> headers) const noexcept {
    const auto normalized_peer = peer_ip.normalized_ipv4();
    if (!enabled_ || trusted_cidrs_.empty() || !is_trusted_proxy(normalized_peer)) {
        return {.address_ = normalized_peer, .from_forwarded_header_ = false};
    }
    for (const auto& header_spec : headers_) {
        const bool present = std::ranges::any_of(headers, [&](const http_header& field) noexcept {
            return http_ascii_equals_ignore_case(field.name(), header_spec.name_);
        });
        if (!present) {
            continue;
        }
        if (const auto extracted = resolve_from_header(
                *this, normalized_peer, header_spec.name_, header_spec.kind_, headers)) {
            return *extracted;
        }
    }
    return {.address_ = normalized_peer, .from_forwarded_header_ = false};
}

http_client_ip_result http_client_ip_extractor::resolve(
    const ip_address& peer_ip, std::span<const http_header_view> headers) const noexcept {
    const auto normalized_peer = peer_ip.normalized_ipv4();
    if (!enabled_ || trusted_cidrs_.empty() || !is_trusted_proxy(normalized_peer)) {
        return {.address_ = normalized_peer, .from_forwarded_header_ = false};
    }
    for (const auto& header_spec : headers_) {
        const bool present =
            std::ranges::any_of(headers, [&](const http_header_view& field) noexcept {
                return http_ascii_equals_ignore_case(field.name(), header_spec.name_);
            });
        if (!present) {
            continue;
        }
        if (const auto extracted = resolve_from_header(
                *this, normalized_peer, header_spec.name_, header_spec.kind_, headers)) {
            return *extracted;
        }
    }
    return {.address_ = normalized_peer, .from_forwarded_header_ = false};
}

http_client_ip_result http_client_ip_extractor::resolve(
    const ip_address& peer_ip, const http_request& request) const noexcept {
    return resolve(peer_ip, request.headers());
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(
    std::string_view peer_ip, std::span<const http_header> headers) const noexcept {
    const auto parsed = ip_address::parse(peer_ip);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return resolve(*parsed, headers);
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(
    std::string_view peer_ip, std::span<const http_header_view> headers) const noexcept {
    const auto parsed = ip_address::parse(peer_ip);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    return resolve(*parsed, headers);
}

std::optional<http_client_ip_result> http_client_ip_extractor::resolve(
    std::string_view peer_ip, const http_request& request) const noexcept {
    return resolve(peer_ip, request.headers());
}

}  // namespace ruvia
