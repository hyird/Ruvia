#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "borrowed_text.h"
#include "http_connection_fields.h"
#include "http_cookie.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "http_wire_writer.h"

namespace ruvia::detail {

template <std::size_t n>
[[nodiscard]] consteval http_response_header static_response_header(std::string_view name, const char (&bytes_value)[n], std::uint32_t known_bit) {
    static_assert(n > 1 && response_header_storage_size_fits(0, n - 1));
    if (name.empty() || name.size() > n - 1 || bytes_value[n - 1] != '\0' || !std::string_view(bytes_value, n - 1).starts_with(name)) {
        throw std::logic_error("invalid static response header storage");
    }
    if (known_bit != 0 && known_bit != response_header_upgrade && response_known_header_slot(known_bit) == response_known_header_count) {
        throw std::logic_error("static response header requires one known header bit");
    }
    return http_response_header_access::make(bytes_value, static_cast<std::uint32_t>(name.size()), static_cast<std::uint32_t>(n - 1 - name.size()), known_bit, false);
}

template <std::size_t... sizes>
[[nodiscard]] consteval auto static_response_headers(std::string_view name, std::uint32_t known_bit, const char (&... bytes_values)[sizes]) {
    return std::array{static_response_header(name, bytes_values, known_bit)...};
}

[[nodiscard]] inline std::optional<http_response_header> find_static_response_header(std::span<const http_response_header> headers, std::string_view value) noexcept {
    for (const auto& header_value : headers) {
        if (header_value.value() == value) {
            return header_value;
        }
    }
    return std::nullopt;
}

inline constexpr auto content_type_headers = static_response_headers("Content-Type", response_header_content_type, "Content-Typetext/plain; charset=UTF-8", "Content-Typeapplication/json", "Content-Typetext/html; charset=UTF-8", "Content-Typetext/plain; charset=utf-8", "Content-Typeapplication/json; charset=utf-8", "Content-Typetext/html; charset=utf-8", "Content-Typetext/css; charset=utf-8", "Content-Typetext/javascript; charset=utf-8", "Content-Typetext/event-stream", "Content-Typeimage/png", "Content-Typeimage/jpeg", "Content-Typeimage/gif", "Content-Typeimage/svg+xml", "Content-Typeapplication/wasm", "Content-Typeapplication/octet-stream", "Content-Typeapplication/problem+json", "Content-Typeimage/apng", "Content-Typeimage/avif", "Content-Typeimage/webp", "Content-Typeimage/bmp", "Content-Typeimage/vnd.microsoft.icon", "Content-Typefont/woff", "Content-Typefont/woff2", "Content-Typefont/ttf", "Content-Typefont/otf", "Content-Typeapplication/vnd.ms-fontobject", "Content-Typetext/xml; charset=utf-8", "Content-Typeapplication/manifest+json; charset=utf-8");

[[nodiscard]] inline http_response_header static_content_type_header(http_content_type type) {
    switch (type) {
        case http_content_type::text_plain_utf8:
            return content_type_headers[0];
        case http_content_type::text_html_utf8:
            return content_type_headers[2];
        case http_content_type::application_json:
            return content_type_headers[1];
    }
    throw std::invalid_argument("invalid HTTP content type");
}

[[nodiscard]] inline std::optional<http_response_header> builtin_static_response_header(std::uint32_t known_bit, std::string_view value) noexcept {
    switch (known_bit) {
        case response_header_content_type:
            return find_static_response_header(content_type_headers, value);
        case response_header_connection: {
            static constexpr auto headers = static_response_headers("Connection", response_header_connection, "Connectionclose", "Connectionkeep-alive", "ConnectionUpgrade");
            return find_static_response_header(headers, value);
        }
        case response_header_accept_ranges: {
            static constexpr auto headers = static_response_headers("Accept-Ranges", response_header_accept_ranges, "Accept-Rangesbytes");
            return find_static_response_header(headers, value);
        }
        case response_header_content_encoding: {
            static constexpr auto headers = static_response_headers("Content-Encoding", response_header_content_encoding, "Content-Encodinggzip", "Content-Encodingbr", "Content-Encodingzstd", "Content-Encodingdeflate");
            return find_static_response_header(headers, value);
        }
        case response_header_transfer_encoding: {
            static constexpr auto headers = static_response_headers("Transfer-Encoding", response_header_transfer_encoding, "Transfer-Encodingchunked");
            return find_static_response_header(headers, value);
        }
        case response_header_cache_control: {
            static constexpr auto headers = static_response_headers("Cache-Control", response_header_cache_control, "Cache-Controlno-store", "Cache-Controlno-cache", "Cache-Controlprivate");
            return find_static_response_header(headers, value);
        }
        case response_header_vary: {
            static constexpr auto headers = static_response_headers("Vary", response_header_vary, "Vary*", "VaryAccept-Encoding", "VaryOrigin", "VaryAccess-Control-Request-Headers", "VaryAccess-Control-Request-Method");
            return find_static_response_header(headers, value);
        }
        case response_header_access_control_allow_origin: {
            static constexpr auto headers = static_response_headers("Access-Control-Allow-Origin", response_header_access_control_allow_origin, "Access-Control-Allow-Origin*");
            return find_static_response_header(headers, value);
        }
        case response_header_access_control_allow_credentials: {
            static constexpr auto headers = static_response_headers("Access-Control-Allow-Credentials", response_header_access_control_allow_credentials, "Access-Control-Allow-Credentialstrue");
            return find_static_response_header(headers, value);
        }
        case response_header_access_control_allow_methods: {
            static constexpr auto headers = static_response_headers("Access-Control-Allow-Methods", response_header_access_control_allow_methods, "Access-Control-Allow-MethodsGET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS");
            return find_static_response_header(headers, value);
        }
        case response_header_upgrade: {
            static constexpr auto headers = static_response_headers("Upgrade", response_header_upgrade, "Upgradewebsocket", "Upgradeconnect-udp");
            return find_static_response_header(headers, value);
        }
        default:
            return std::nullopt;
    }
}

[[nodiscard]] inline std::optional<http_response_header> builtin_static_custom_response_header(std::string_view name, std::string_view value) noexcept {
    switch (name.size()) {
        case std::string_view("Retry-After").size():
            if (name == "Retry-After") {
                static constexpr auto headers = static_response_headers("Retry-After", 0, "Retry-After1");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-Frame-Options").size():
            if (name == "X-Frame-Options") {
                static constexpr auto headers = static_response_headers("X-Frame-Options", 0, "X-Frame-OptionsDENY", "X-Frame-OptionsSAMEORIGIN");
                return find_static_response_header(headers, value);
            }
            if (name == "Referrer-Policy") {
                static constexpr auto headers = static_response_headers("Referrer-Policy", 0, "Referrer-Policystrict-origin-when-cross-origin", "Referrer-Policyno-referrer");
                return find_static_response_header(headers, value);
            }
            if (name == "Accept-Encoding") {
                static constexpr auto headers = static_response_headers("Accept-Encoding", 0, "Accept-Encodinggzip, deflate, br, zstd", "Accept-Encodingidentity");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-XSS-Protection").size():
            if (name == "X-XSS-Protection") {
                static constexpr auto headers = static_response_headers("X-XSS-Protection", 0, "X-XSS-Protection0");
                return find_static_response_header(headers, value);
            }
            if (name == "Capsule-Protocol") {
                static constexpr auto headers = static_response_headers("Capsule-Protocol", 0, "Capsule-Protocol?1");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-RateLimit-Reset").size():
            if (name == "X-RateLimit-Reset") {
                static constexpr auto headers = static_response_headers("X-RateLimit-Reset", 0, "X-RateLimit-Reset1");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("Permissions-Policy").size():
            if (name == "Permissions-Policy") {
                static constexpr auto headers = static_response_headers("Permissions-Policy", 0, "Permissions-Policygeolocation=(), microphone=(), camera=()");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-RateLimit-Remaining").size():
            if (name == "X-RateLimit-Remaining") {
                static constexpr auto headers = static_response_headers("X-RateLimit-Remaining", 0, "X-RateLimit-Remaining0");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("X-Content-Type-Options").size():
            if (name == "X-Content-Type-Options") {
                static constexpr auto headers = static_response_headers("X-Content-Type-Options", 0, "X-Content-Type-Optionsnosniff");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("Content-Security-Policy").size():
            if (name == "Content-Security-Policy") {
                static constexpr auto headers = static_response_headers("Content-Security-Policy", 0, "Content-Security-Policydefault-src 'self'", "Content-Security-Policydefault-src 'none'");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        case std::string_view("Strict-Transport-Security").size():
            if (name == "Strict-Transport-Security") {
                static constexpr auto headers = static_response_headers("Strict-Transport-Security", 0, "Strict-Transport-Securitymax-age=31536000; includeSubDomains");
                return find_static_response_header(headers, value);
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

}  // namespace ruvia::detail

namespace ruvia {

http_response_headers::http_response_headers() noexcept = default;

http_response_headers::~http_response_headers() {
    clear();
}

http_response_headers::http_response_headers(http_response_headers&& other) noexcept {
    move_from(std::move(other));
}

void http_response_headers::reserve(std::size_t count) {
    if (count <= inline_capacity) {
        return;
    }

    if (!spilled_) {
        spill(count);
        return;
    }
    if (count <= heap_.capacity()) {
        return;
    }
    // Only descriptors move; field bytes retain their existing ownership.
    std::vector<http_response_header> replacement;
    replacement.reserve(count);
    replacement.insert(replacement.end(), heap_.begin(), heap_.end());
    heap_.swap(replacement);
}

http_response_header& http_response_headers::append_prepared_header(http_response_header header_value) noexcept {
    if (!spilled_) {
        if (size_ == inline_capacity) {
            std::terminate();
        }
        auto* const target = inline_data() + size_;
        *target = header_value;
        ++size_;
        return *target;
    }

    if (heap_.size() == heap_.capacity()) {
        std::terminate();
    }
    heap_.push_back(header_value);
    return heap_.back();
}

http_response_header* http_response_headers::inline_data() noexcept {
    return reinterpret_cast<http_response_header*>(inline_.data());
}

const http_response_header* http_response_headers::inline_data() const noexcept {
    return reinterpret_cast<const http_response_header*>(inline_.data());
}

http_response_header* http_response_headers::data() noexcept {
    return spilled_ ? heap_.data() : inline_data();
}

const http_response_header* http_response_headers::data() const noexcept {
    return spilled_ ? heap_.data() : inline_data();
}

void http_response_headers::clear() noexcept {
    auto* items = data();
    const auto count = size();
    for (std::size_t i = 0; i < count; ++i) {
        release_header(items[i]);
    }
    if (spilled_) {
        heap_.clear();
    }
    size_ = 0;
}

void http_response_headers::spill(std::size_t min_capacity) {
    heap_.clear();
    heap_.reserve(std::max<std::size_t>(inline_capacity * 2, min_capacity));
    auto* items = inline_data();
    for (std::size_t i = 0; i < size_; ++i) {
        heap_.push_back(items[i]);
    }
    size_ = heap_.size();
    spilled_ = true;
}

void http_response_headers::move_from(http_response_headers&& other) noexcept {
    known_header_bits_ = std::exchange(other.known_header_bits_, 0U);
    if (known_header_bits_ != 0) {
        known_header_indexes_ = other.known_header_indexes_;
    }
    if (other.spilled_) {
        spilled_ = true;
        heap_ = std::move(other.heap_);
        size_ = heap_.size();
        other.spilled_ = false;
        other.size_ = 0;
        return;
    }

    if (other.size_ > 0) {
        std::memcpy(inline_.data(), other.inline_.data(), other.size_ * sizeof(inline_storage_type));
    }
    size_ = other.size_;
    other.size_ = 0;
}

bool http_response_headers::borrows_owned_header(std::string_view name, std::string_view value) const noexcept {
    for (const auto& existing : *this) {
        if (existing.owned_ && (detail::response_header_storage_overlaps(existing, name) || detail::response_header_storage_overlaps(existing, value))) {
            return true;
        }
    }
    return false;
}
http_response_header http_response_headers::make_owned_header(std::string_view name, std::string_view value, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value.size());
    const auto total = name.size() + value.size();
    char* bytes_value = nullptr;
    if (total > 0) {
        bytes_value = new char[total];
        std::memcpy(bytes_value, name.data(), name.size());
        std::memcpy(bytes_value + name.size(), value.data(), value.size());
    }
    return detail::http_response_header_access::make(bytes_value, static_cast<std::uint32_t>(name.size()), static_cast<std::uint32_t>(value.size()), known_bit, true);
}

http_response_header http_response_headers::make_uninitialized_header(std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value_size);
    const auto total = name.size() + value_size;
    char* bytes_value = nullptr;
    if (total > 0) {
        bytes_value = new char[total];
        std::memcpy(bytes_value, name.data(), name.size());
    }
    return detail::http_response_header_access::make(bytes_value, static_cast<std::uint32_t>(name.size()), static_cast<std::uint32_t>(value_size), known_bit, true);
}

std::optional<http_response_header> http_response_headers::make_static_header(std::string_view name, std::string_view value, std::uint32_t known_bit) noexcept {
    if (known_bit == 0) {
        return detail::builtin_static_custom_response_header(name, value);
    }
    auto header_value = detail::builtin_static_response_header(known_bit, value);
    if (!header_value || header_value->name() != name) {
        return std::nullopt;
    }
    return header_value;
}

std::optional<http_response_header> http_response_headers::make_shared_header(std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view) const noexcept {
    if (const auto static_header = make_static_header(name, value, known_bit)) {
        return static_header;
    }
    if (stable_view && !borrows_owned_header(name, value) && detail::response_header_views_contiguous(name, value) && detail::response_header_storage_size_fits(name.size(), value.size())) {
        return detail::http_response_header_access::make(name.data(), static_cast<std::uint32_t>(name.size()), static_cast<std::uint32_t>(value.size()), known_bit, false);
    }
    return std::nullopt;
}

void http_response_headers::release_header(http_response_header& header_value) noexcept {
    if (header_value.owned_ && header_value.bytes_ != nullptr) {
        delete[] const_cast<char*>(header_value.bytes_);
    }
    header_value.bytes_ = nullptr;
    header_value.name_size_ = 0;
    header_value.value_size_ = 0;
    header_value.known_bit_ = 0;
    header_value.owned_ = false;
    header_value.append_ = false;
}

void http_response_headers::truncate(iterator new_end) noexcept {
    if (spilled_) {
        heap_.erase(heap_.begin() + (new_end - heap_.data()), heap_.end());
        return;
    }
    size_ = static_cast<std::size_t>(new_end - inline_data());
}

http_response_header& http_response_headers::append_header(http_response_header header_value) {
    // `header` may own a separately allocated name/value block.  The vector
    // stores the small descriptor by value, so an exception while spilling or
    // appending must release that block here; otherwise a failed heap
    // allocation leaks the response header and leaves the next retry with a
    // different ownership picture.
    try {
        if (!spilled_ && size_ == inline_capacity) {
            spill(size_ + 1);
        }
        if (!spilled_) {
            auto* target = inline_data() + size_;
            *target = header_value;
            ++size_;
            return *target;
        }
        heap_.push_back(header_value);
        return heap_.back();
    } catch (...) {
        release_header(header_value);
        throw;
    }
}

http_response_header& http_response_headers::add(std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view) {
    const auto shared_header = make_shared_header(name, value, known_bit, stable_view);
    return append_header(shared_header ? *shared_header : make_owned_header(name, value, known_bit));
}

http_response_header& http_response_headers::add_uninitialized_value(std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
    return append_header(make_uninitialized_header(name, value_size, known_bit));
}

http_response_header& http_response_headers::assign_uninitialized_value(http_response_header& header_value, std::string_view name, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(name.size(), value_size);
    const auto total = name.size() + value_size;
    if (header_value.owned_ && header_value.bytes_ != nullptr && !detail::response_header_storage_overlaps(header_value, name) && total == static_cast<std::size_t>(header_value.name_size_) + header_value.value_size_) {
        auto* const bytes_value = const_cast<char*>(header_value.bytes_);
        std::memcpy(bytes_value, name.data(), name.size());
        header_value.name_size_ = static_cast<std::uint32_t>(name.size());
        header_value.value_size_ = static_cast<std::uint32_t>(value_size);
        header_value.known_bit_ = known_bit;
        header_value.append_ = false;
        return header_value;
    }

    const auto replacement = make_uninitialized_header(name, value_size, known_bit);
    release_header(header_value);
    header_value = replacement;
    return header_value;
}

bool http_response_headers::try_assign_owned_in_place(http_response_header& header_value, std::string_view name, std::string_view value, std::uint32_t known_bit) noexcept {
    if (!detail::response_header_storage_size_fits(name.size(), value.size())) {
        return false;
    }
    const auto total = name.size() + value.size();
    if (!header_value.owned_ || header_value.bytes_ == nullptr || detail::response_header_storage_overlaps(header_value, name) || detail::response_header_storage_overlaps(header_value, value) || total != static_cast<std::size_t>(header_value.name_size_) + header_value.value_size_) {
        return false;
    }
    auto* const bytes_value = const_cast<char*>(header_value.bytes_);
    std::memcpy(bytes_value, name.data(), name.size());
    std::memcpy(bytes_value + name.size(), value.data(), value.size());
    header_value.name_size_ = static_cast<std::uint32_t>(name.size());
    header_value.value_size_ = static_cast<std::uint32_t>(value.size());
    header_value.known_bit_ = known_bit;
    header_value.append_ = false;
    return true;
}

void http_response_headers::assign(http_response_header& header_value, std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view) {
    if (const auto shared_header = make_shared_header(name, value, known_bit, stable_view)) {
        release_header(header_value);
        header_value = *shared_header;
        return;
    }
    if (try_assign_owned_in_place(header_value, name, value, known_bit)) {
        return;
    }
    const auto replacement = make_owned_header(name, value, known_bit);
    release_header(header_value);
    header_value = replacement;
}

namespace {

inline constexpr std::size_t allow_header_method_slots = static_cast<std::size_t>(http_known_method::unknown);

void write_content_range_header_value(http_response_header& header_value, std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    detail::http_raw_wire_sink sink(detail::http_response_header_access::value_begin(header_value));
    sink.append("bytes ");
    sink.append_unsigned(offset);
    sink.append('-');
    sink.append_unsigned(offset + length - 1);
    sink.append('/');
    sink.append_unsigned(size);
    if (sink.position() != detail::http_response_header_access::value_end(header_value)) {
        throw std::logic_error("failed to format HTTP Content-Range header");
    }
}

void write_content_range_unsatisfied_header_value(http_response_header& header_value, std::uint64_t size) {
    detail::http_raw_wire_sink sink(detail::http_response_header_access::value_begin(header_value));
    sink.append("bytes */");
    sink.append_unsigned(size);
    if (sink.position() != detail::http_response_header_access::value_end(header_value)) {
        throw std::logic_error("failed to format HTTP Content-Range header");
    }
}

[[nodiscard]] std::size_t allow_header_value_size(std::uint32_t method_mask, std::span<const std::string_view> extension_methods) {
    std::size_t size = 0;
    std::size_t count = 0;
    const auto add_method = [&](std::size_t token_size) {
        const auto separator_size = count == 0 ? std::size_t{0} : std::size_t{2};
        detail::validate_response_header_storage_size(std::string_view("Allow").size() + size, separator_size);
        size += separator_size;
        detail::validate_response_header_storage_size(std::string_view("Allow").size() + size, token_size);
        size += token_size;
        ++count;
    };
    for (std::size_t i = 0; i < allow_header_method_slots; ++i) {
        if ((method_mask & (1U << i)) == 0) {
            continue;
        }
        add_method(known_http_method_token(static_cast<http_known_method>(i)).size());
    }
    for (const auto token : extension_methods) {
        add_method(token.size());
    }
    return size;
}

void write_allow_header_value(http_response_header& header_value, std::uint32_t method_mask, std::span<const std::string_view> extension_methods) {
    detail::http_raw_wire_sink sink(detail::http_response_header_access::value_begin(header_value));
    bool first = true;
    for (std::size_t i = 0; i < allow_header_method_slots; ++i) {
        if ((method_mask & (1U << i)) == 0) {
            continue;
        }
        if (!first) {
            sink.append(", ");
        }
        first = false;
        sink.append(known_http_method_token(static_cast<http_known_method>(i)));
    }
    for (const auto token : extension_methods) {
        if (!first) {
            sink.append(", ");
        }
        first = false;
        sink.append(token);
    }
    if (sink.position() != detail::http_response_header_access::value_end(header_value)) {
        throw std::logic_error("failed to format HTTP Allow header");
    }
}

[[nodiscard]] bool set_cookie_wire_name_matches(std::string_view value, std::string_view wire_prefix, std::string_view cookie_name) noexcept {
    if (value.size() != wire_prefix.size() + cookie_name.size()) {
        return false;
    }
    if (!value.starts_with(wire_prefix)) {
        return false;
    }
    return value.substr(wire_prefix.size()) == cookie_name;
}

[[nodiscard]] bool set_cookie_value_matches_storage_key(std::string_view value, std::string_view wire_prefix, std::string_view cookie_name, bool has_path, std::string_view path, std::string_view domain, bool partitioned) noexcept {
    const auto parsed_value = parse_set_cookie(value);
    return parsed_value.has_value() && is_valid_http_header_name(parsed_value->name()) && set_cookie_wire_name_matches(parsed_value->name(), wire_prefix, cookie_name) && !parsed_value->path().empty() == has_path && parsed_value->has(http_set_cookie_attribute::partitioned) == partitioned && (!has_path || parsed_value->path() == path) && http_ascii_equals_ignore_case(parsed_value->domain(), domain);
}

}  // namespace

void http_response_headers::allow_methods(std::uint32_t method_mask, std::span<const std::string_view> extension_methods) {
    const auto value_size = allow_header_value_size(method_mask, extension_methods);
    for (const auto token : extension_methods) {
        if (!is_valid_http_method_token(token)) {
            throw std::invalid_argument("invalid HTTP Allow method");
        }
    }
    if (!extension_methods.empty()) {
        if (auto* const retained = find_header_for_update("Allow", detail::response_header_allow)) {
            bool borrows_allow = false;
            const auto overlaps_retained = [&](std::string_view token) noexcept {
                if (!detail::http_response_header_access::append(*retained)) {
                    return detail::response_header_storage_overlaps(*retained, token);
                }
                for (const auto& existing : *this) {
                    if (detail::http_response_header_access::known_bit(existing) == detail::response_header_allow && detail::response_header_storage_overlaps(existing, token)) {
                        return true;
                    }
                }
                return false;
            };
            for (const auto token : extension_methods) {
                if (overlaps_retained(token)) {
                    borrows_allow = true;
                    break;
                }
            }
            if (borrows_allow) {
                auto prepared = make_uninitialized_header("Allow", value_size, detail::response_header_allow);
                try {
                    write_allow_header_value(prepared, method_mask, extension_methods);
                } catch (...) {
                    release_header(prepared);
                    throw;
                }
                (void)replace_header(*retained, prepared);
                return;
            }
        }
    }
    auto& header_value = prepare_header_value_storage("Allow", value_size, detail::response_header_allow);
    write_allow_header_value(header_value, method_mask, extension_methods);
}

void http_response_headers::content_range(http_content_length offset_value, http_content_length length_value, http_content_length size_value) {
    const auto offset = offset_value.value();
    const auto length = length_value.value();
    const auto size = size_value.value();
    if (length == 0) {
        throw std::logic_error("file response byte range length must not be zero");
    }
    if (offset > size || length > size - offset) {
        throw std::logic_error("file response byte range is outside the representation");
    }
    const auto end_offset = offset + length - 1;
    const auto value_size = std::string_view("bytes ").size() + http_unsigned_decimal_size(offset) + 1 + http_unsigned_decimal_size(end_offset) + 1 + http_unsigned_decimal_size(size);
    auto& header_value = prepare_header_value_storage("Content-Range", value_size, detail::response_header_content_range);
    write_content_range_header_value(header_value, offset, length, size);
}

void http_response_headers::content_range_unsatisfied(http_content_length size_value) {
    const auto size = size_value.value();
    const auto value_size = std::string_view("bytes */").size() + http_unsigned_decimal_size(size);
    auto& header_value = prepare_header_value_storage("Content-Range", value_size, detail::response_header_content_range);
    write_content_range_unsatisfied_header_value(header_value, size);
}

void http_response_headers::set_cookie(const set_cookie_plan& plan) {
    auto* retained = find_set_cookie_header(plan.wire_prefix(), plan.name(), !plan.path().empty(), plan.path(), plan.domain(), plan.partitioned_);
    if (retained == nullptr) {
        auto& header_value = append_header_uninitialized_value("Set-Cookie", plan.size(), detail::response_header_set_cookie);
        plan.write(detail::http_response_header_access::value_begin(header_value));
        return;
    }

    const bool borrows_retained = detail::response_header_storage_overlaps(*retained, plan.name_) || detail::response_header_storage_overlaps(*retained, plan.value_) || detail::response_header_storage_overlaps(*retained, plan.path_) || detail::response_header_storage_overlaps(*retained, plan.domain_);
    set_cookie_plan::written_fields written;
    if (borrows_retained) {
        auto prepared = make_uninitialized_header("Set-Cookie", plan.size(), detail::response_header_set_cookie);
        written = plan.write_fields(detail::http_response_header_access::value_begin(prepared));
        release_header(*retained);
        *retained = prepared;
    } else {
        assign_uninitialized_value(*retained, "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        written = plan.write_fields(detail::http_response_header_access::value_begin(*retained));
    }
    detail::http_response_header_access::set_append(*retained);
    erase_later_set_cookie_headers(*retained, written.wire_name_, !written.path_.empty(), written.path_, written.domain_, plan.partitioned_);
}

void http_response_headers::upsert_set_cookie_header_validated(std::string_view value) {
    const auto parsed_value = parse_set_cookie(value);
    const auto cookie_name = parsed_value.has_value() && is_valid_http_header_name(parsed_value->name()) ? parsed_value->name() : std::string_view{};
    if (cookie_name.empty()) {
        append_header_validated("Set-Cookie", value, detail::response_header_set_cookie);
        return;
    }

    const auto has_path = !parsed_value->path().empty();
    const auto partitioned = parsed_value->has(http_set_cookie_attribute::partitioned);
    auto* retained = find_set_cookie_header({}, cookie_name, has_path, parsed_value->path(), parsed_value->domain(), partitioned);
    if (retained == nullptr) {
        append_header_validated("Set-Cookie", value, detail::response_header_set_cookie);
        return;
    }

    const auto name_size = cookie_name.size();
    const auto path_size = parsed_value->path().size();
    const auto domain_size = parsed_value->domain().size();
    const auto name_offset = static_cast<std::size_t>(cookie_name.data() - value.data());
    const auto path_offset = parsed_value->path().empty() ? std::size_t{0} : static_cast<std::size_t>(parsed_value->path().data() - value.data());
    const auto domain_offset = parsed_value->domain().empty() ? std::size_t{0} : static_cast<std::size_t>(parsed_value->domain().data() - value.data());
    assign(*retained, "Set-Cookie", value, detail::response_header_set_cookie);
    detail::http_response_header_access::set_append(*retained);
    const auto copied_value = retained->value();
    erase_later_set_cookie_headers(*retained, copied_value.substr(name_offset, name_size), has_path, copied_value.substr(path_offset, path_size), copied_value.substr(domain_offset, domain_size), partitioned);
}

http_response_header* http_response_headers::find_set_cookie_header(std::string_view wire_prefix, std::string_view cookie_name, bool has_path, std::string_view path, std::string_view domain, bool partitioned) noexcept {
    for (auto* header = mutable_begin(); header != mutable_end(); ++header) {
        if (detail::http_response_header_access::known_bit(*header) == detail::response_header_set_cookie && set_cookie_value_matches_storage_key(header->value(), wire_prefix, cookie_name, has_path, path, domain, partitioned)) {
            return header;
        }
    }
    return nullptr;
}

void http_response_headers::erase_later_set_cookie_headers(http_response_header& retained, std::string_view cookie_name, bool has_path, std::string_view path, std::string_view domain, bool partitioned) noexcept {
    auto* const end_value = mutable_end();
    auto* write = &retained + 1;
    for (auto* read = write; read != end_value; ++read) {
        if (detail::http_response_header_access::known_bit(*read) == detail::response_header_set_cookie && set_cookie_value_matches_storage_key(read->value(), {}, cookie_name, has_path, path, domain, partitioned)) {
            release_header(*read);
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    if (write != end_value) {
        truncate(write);
        rebuild_known_header_index();
    }
}

namespace {

// Every insertion path validates one field line completely, so a serialized
// head only re-checks the rules that span several lines. Returns the field's
// known bit.
[[nodiscard]] std::uint32_t validate_response_header_field(std::string_view key, std::string_view value) {
    // Check the descriptor's representable storage before any grammar scan;
    // callers may provide a view over a bounded buffer with a hostile length.
    detail::validate_response_header_storage_size(key.size(), value.size());
    if (!is_valid_http_header_name(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!is_valid_http_header_value(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    const auto field = detail::classify_http_field_name(key);
    const auto known_bit = field.response_bit_;
    const auto kind = field.kind_;
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    switch (detail::parse_http_response_control_field(kind, value, connection_options, upgrade_protocols)) {
        case detail::http_response_control_field_status::invalid_connection:
            throw std::invalid_argument("invalid HTTP Connection header");
        case detail::http_response_control_field_status::invalid_upgrade:
            throw std::invalid_argument("invalid HTTP Upgrade header");
        case detail::http_response_control_field_status::te_forbidden:
            throw std::invalid_argument("TE is not a response field");
        case detail::http_response_control_field_status::ok:
            break;
    }
    detail::response_field_state fields;
    if (detail::validate_response_field(kind, value, fields) != detail::response_field_status::ok) {
        throw std::invalid_argument("invalid HTTP response field value");
    }
    return known_bit;
}

[[nodiscard]] bool response_header_mode_appends(http_response_header_mode mode) {
    switch (mode) {
        case http_response_header_mode::replace:
            return false;
        case http_response_header_mode::append:
            return true;
    }
    throw std::invalid_argument("invalid HTTP response header mode");
}

}  // namespace

http_response_headers http_response_headers::clone(std::size_t additional_headers) const {
    const auto existing = size();
    if (additional_headers > heap_.max_size() - existing) {
        throw std::length_error("HTTP response header table is too large");
    }
    http_response_headers copy;
    copy.reserve(existing + additional_headers);
    for (const auto& header_value : *this) {
        // Static descriptors can be shared; owning descriptors must keep an
        // independent allocation so either block may be changed or destroyed.
        auto descriptor = header_value.owned_ ? copy.make_owned_header(header_value.name(), header_value.value(), header_value.known_bit_) : header_value;
        descriptor.append_ = header_value.append_;
        (void)copy.append_prepared_header(descriptor);
    }
    copy.known_header_bits_ = known_header_bits_;
    if (known_header_bits_ != 0) {
        copy.known_header_indexes_ = known_header_indexes_;
    }
    return copy;
}

void http_response_headers::commit(http_response_headers&& staged) noexcept {
    if (this == &staged) {
        return;
    }
    // Reconstruction adopts the staged table without allocating; the move
    // leaves `staged` empty with its own resource and a cleared index.
    std::destroy_at(this);
    std::construct_at(this, std::move(staged));
}

bool http_response_headers::try_append_disjoint(const http_response_headers& source, http_response_header_transfer mode) {
    if (&source == this) {
        return false;
    }
    const auto skips = [mode](std::uint32_t known_bit) noexcept {
        return mode == http_response_header_transfer::assign && known_bit == detail::response_header_content_type;
    };
    // A source block already holds each field under its own rules: one line per
    // single-valued field and key-unique cookies. When no source field names a
    // field this block holds, every transfer mode keeps each source line as is.
    for (const auto& header_value : source) {
        const auto known_bit = detail::http_response_header_access::known_bit(header_value);
        if (skips(known_bit)) {
            continue;
        }
        if (known_bit != 0 ? (known_header_bits_ & known_bit) != 0 : find_header_for_read(header_value.name(), 0) != nullptr) {
            return false;
        }
    }

    reserve(size() + source.size());
    const auto first_index = size();
    try {
        for (const auto& header_value : source) {
            const auto known_bit = header_value.known_bit_;
            if (skips(known_bit)) {
                continue;
            }
            // Static descriptors are shared; owning descriptors get an
            // independent allocation, as in clone().
            auto descriptor = header_value.owned_ ? make_owned_header(header_value.name(), header_value.value(), known_bit) : header_value;
            // Transferred cookies are appended occurrences, like the staged path.
            descriptor.append_ = header_value.append_ || known_bit == detail::response_header_set_cookie;
            (void)append_prepared_header(descriptor);
        }
    } catch (...) {
        auto* const first = data() + first_index;
        for (auto* cursor_value = first; cursor_value != end(); ++cursor_value) {
            release_header(*cursor_value);
        }
        truncate(first);
        throw;
    }
    const auto* const items = data();
    for (auto index = first_index; index < size(); ++index) {
        if (const auto known_bit = items[index].known_bit_; known_bit != 0) {
            record_known_header_index(known_bit, index);
        }
    }
    return true;
}

void http_response_headers::transfer_from(const http_response_headers& source, http_response_header_transfer mode) {
    switch (mode) {
        case http_response_header_transfer::merge:
        case http_response_header_transfer::assign:
        case http_response_header_transfer::apply:
            break;
        default:
            throw std::invalid_argument("invalid HTTP response header transfer mode");
    }
    if (try_append_disjoint(source, mode)) {
        return;
    }
    // Fields that meet existing ones are resolved on a staged copy, so a
    // failure leaves this block unchanged.
    auto staged = clone(source.size());
    const auto original_size = staged.size();
    bool removed_cookies = false;
    for (const auto& header_value : source) {
        const auto known_bit = detail::http_response_header_access::known_bit(header_value);
        if (mode == http_response_header_transfer::assign && known_bit == detail::response_header_content_type) {
            continue;
        }
        const auto name = header_value.name();
        const auto value = header_value.value();
        if (known_bit == detail::response_header_set_cookie) {
            if (mode == http_response_header_transfer::assign && !removed_cookies) {
                (void)staged.remove_header_validated("Set-Cookie", detail::response_header_set_cookie);
                removed_cookies = true;
            }
            staged.upsert_set_cookie_header_validated(value);
        } else if (detail::http_response_header_access::append(header_value)) {
            const auto matches_header = [&](const http_response_header& candidate) noexcept {
                return detail::response_header_matches(candidate, name, known_bit) && candidate.value() == value;
            };
            const auto occurrence_through = [&] {
                std::size_t count = 0;
                for (const auto& candidate : source) {
                    if (matches_header(candidate)) {
                        ++count;
                    }
                    if (&candidate == &header_value) {
                        break;
                    }
                }
                return count;
            }();
            const auto existing_count = [&] {
                std::size_t count = 0;
                const auto* const items = staged.data();
                for (std::size_t index = 0, limit = std::min(original_size, staged.size()); index < limit; ++index) {
                    if (matches_header(items[index])) {
                        ++count;
                    }
                }
                return count;
            }();
            if (existing_count < occurrence_through) {
                staged.append_header_validated(name, value, known_bit);
            }
        } else if (mode == http_response_header_transfer::merge) {
            if (staged.find_header_for_read(name, known_bit) == nullptr) {
                if (!header_value.owned_) {
                    staged.publish_header(header_value);
                } else {
                    staged.set_header_validated(name, value, known_bit);
                }
            }
        } else if (!header_value.owned_) {
            staged.publish_header(header_value);
        } else {
            staged.set_header_validated(name, value, known_bit);
        }
    }
    commit(std::move(staged));
}

void http_response_headers::record_known_header_index(std::uint32_t known_bit, std::size_t index) noexcept {
    if (known_bit == 0 || (known_header_bits_ & known_bit) != 0) {
        return;
    }
    known_header_bits_ |= known_bit;
    const auto slot = detail::response_known_header_slot(known_bit);
    if (slot >= known_header_indexes_.size()) {
        return;
    }
    // A slot holds the first occurrence's index + 1; 0 (unset) and -1 (index
    // beyond int16) both fall back to a scan.
    known_header_indexes_[slot] = index < static_cast<std::size_t>((std::numeric_limits<std::int16_t>::max)()) ? static_cast<std::int16_t>(index + 1) : std::int16_t{-1};
}

http_response_header* http_response_headers::find_header_for_update(std::string_view key, std::uint32_t known_bit) noexcept {
    return const_cast<http_response_header*>(std::as_const(*this).find_header_for_read(key, known_bit));
}

const http_response_header* http_response_headers::find_header_for_read(std::string_view key, std::uint32_t known_bit) const noexcept {
    if (known_bit != 0 && (known_header_bits_ & known_bit) == 0) {
        return nullptr;
    }
    if (const auto slot = detail::response_known_header_slot(known_bit); slot < known_header_indexes_.size() && known_header_indexes_[slot] > 0) {
        return begin() + (known_header_indexes_[slot] - 1);
    }
    for (const auto& header_value : *this) {
        if (detail::response_header_matches(header_value, key, known_bit)) {
            return &header_value;
        }
    }
    return nullptr;
}

template <typename Mutation>
http_response_header& http_response_headers::upsert_header(std::string_view key, std::uint32_t known_bit, Mutation&& mutate) {
    if (auto* const existing = find_header_for_update(key, known_bit)) {
        const bool was_appended = detail::http_response_header_access::append(*existing);
        auto& header_value = mutate(existing);
        return was_appended ? collapse_response_headers(header_value, known_bit) : header_value;
    }
    const auto index = size();
    auto& header_value = mutate(nullptr);
    record_known_header_index(known_bit, index);
    return header_value;
}

http_response_header& http_response_headers::prepare_header_value_storage(std::string_view key, std::size_t value_size, std::uint32_t known_bit) {
    return upsert_header(key, known_bit, [&](http_response_header* existing) -> http_response_header& {
        return existing != nullptr ? assign_uninitialized_value(*existing, key, value_size, known_bit) : add_uninitialized_value(key, value_size, known_bit);
    });
}

std::optional<std::string_view> http_response_headers::header(std::string_view name) const& noexcept {
    if (const auto* const found = find_header_for_read(name, detail::classify_response_header_name(name))) {
        return found->value();
    }
    return std::nullopt;
}

void http_response_headers::rebuild_known_header_index() noexcept {
    known_header_bits_ = 0;
    const auto* const first = data();
    const auto* const last = first + size();
    for (auto* cursor_value = first; cursor_value != last; ++cursor_value) {
        const auto known_bit = detail::http_response_header_access::known_bit(*cursor_value);
        if (known_bit != 0) {
            record_known_header_index(known_bit, static_cast<std::size_t>(cursor_value - first));
        }
    }
}

void http_response_headers::header(std::string_view key, std::string_view value) {
    header(key, value, {});
}

void http_response_headers::content_type(http_content_type type) {
    publish_header(detail::static_content_type_header(type));
}

http_response_header& http_response_headers::replace_header(http_response_header& existing, http_response_header prepared) noexcept {
    const bool was_appended = detail::http_response_header_access::append(existing);
    release_header(existing);
    existing = prepared;
    return was_appended ? collapse_response_headers(existing, prepared.known_bit_) : existing;
}

void http_response_headers::publish_header(http_response_header prepared) {
    (void)upsert_header(prepared.name(), prepared.known_bit_, [&](http_response_header* existing) -> http_response_header& {
        if (existing != nullptr) {
            release_header(*existing);
            *existing = prepared;
            return *existing;
        }
        return append_header(prepared);
    });
}

void http_response_headers::header(std::string_view key, std::string_view value, http_response_header_options options) {
    const auto known_bit = validate_response_header_field(key, value);
    if (response_header_mode_appends(options.mode_)) {
        if (detail::response_header_append_forbidden(known_bit)) {
            throw std::invalid_argument("HTTP response header cannot be appended");
        }
        if (known_bit == detail::response_header_set_cookie) {
            upsert_set_cookie_header_validated(value);
        } else {
            append_header_validated(key, value, known_bit);
        }
    } else {
        set_header_validated(key, value, known_bit);
    }
}

void http_response_headers::remove_header(std::string_view key) {
    detail::validate_response_header_storage_size(key.size(), 0);
    if (!is_valid_http_header_name(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    (void)remove_header_validated(key, detail::classify_response_header_name(key));
}

void http_response_headers::set_header_validated(std::string_view key, std::string_view value, std::uint32_t known_bit, bool stable_view) {
    (void)upsert_header(key, known_bit, [&](http_response_header* existing) -> http_response_header& {
        if (existing != nullptr) {
            assign(*existing, key, value, known_bit, stable_view);
            return *existing;
        }
        return add(key, value, known_bit, stable_view);
    });
}

void http_response_headers::append_header_validated(std::string_view key, std::string_view value, std::uint32_t known_bit) {
    auto& header_value = append_header_uninitialized_value(key, value.size(), known_bit);
    // Appending can move descriptors, but existing header byte blocks stay
    // stable, including any bytes borrowed by value.
    if (!value.empty()) {
        std::memcpy(detail::http_response_header_access::value_begin(header_value), value.data(), value.size());
    }
}

http_response_header& http_response_headers::append_header_uninitialized_value(std::string_view key, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(key.size(), value_size);
    if (detail::response_header_append_forbidden(known_bit)) {
        throw std::invalid_argument("HTTP response header cannot be appended");
    }
    // The index cache intentionally points at the first occurrence. Mark that
    // retained slot as plural too, so a later plain set can detect multiplicity
    // in O(1) and collapse the field without scanning every normal update. Do
    // not set it until the new descriptor has been published: the new header
    // owns bytes before it may allocate the backing table, and a failed append
    // must leave the existing block exactly as it was.
    const auto* const existing = find_header_for_read(key, known_bit);
    const auto existing_index = existing == nullptr ? std::optional<std::size_t>{} : std::optional{static_cast<std::size_t>(existing - data())};
    const auto index = size();
    auto& header_value = add_uninitialized_value(key, value_size, known_bit);
    if (existing_index) {
        // Appending may move the descriptor table. Preserve an index, not a pointer,
        // to mark the existing entry without repeating the header lookup.
        detail::http_response_header_access::set_append(data()[*existing_index]);
    }
    // Mark the append flag so a later merge of this block keeps every appended
    // value instead of treating the field as single-valued and dropping all but the
    // first.
    detail::http_response_header_access::set_append(header_value);
    record_known_header_index(known_bit, index);
    return header_value;
}

http_response_header& http_response_headers::collapse_response_headers(http_response_header& retained, std::uint32_t known_bit) noexcept {
    const auto key = retained.name();
    auto* const first = data();
    auto* const last = first + size();
    auto* const retained_address = &retained;
    auto* collapsed_retained = retained_address;
    auto* write = first;
    for (auto* read = first; read != last; ++read) {
        if (read != retained_address && detail::response_header_matches(*read, key, known_bit)) {
            release_header(*read);
            continue;
        }
        if (read == retained_address) {
            collapsed_retained = write;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    if (write != last) {
        truncate(write);
    }
    rebuild_known_header_index();
    return *collapsed_retained;
}

bool http_response_headers::remove_header_validated(std::string_view key, std::uint32_t known_bit) noexcept {
    if (known_bit != 0 && (known_header_bits_ & known_bit) == 0) {
        return false;
    }
    auto* const first = data();
    auto* const last = first + size();
    auto* write = first;
    bool removed = false;
    http_response_header retired_key_header;

    for (auto* read = first; read != last; ++read) {
        if (detail::response_header_matches(*read, key, known_bit)) {
            if (!removed) {
                retired_key_header = std::exchange(*read, http_response_header{});
                key = retired_key_header.name();
            } else {
                release_header(*read);
            }
            removed = true;
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }

    if (!removed) {
        return false;
    }

    truncate(write);
    rebuild_known_header_index();
    release_header(retired_key_header);
    return true;
}

void http_response_headers::header_stable_view(std::string_view key, std::string_view value) {
    const auto known_bit = validate_response_header_field(key, value);
    set_header_validated(key, value, known_bit, true);
}

void http_response_headers::add_vary_token(std::string_view token) {
    // Vary is a list field (RFC 9110 section 12.5.5): wildcard and membership
    // are decided across every line, not only the indexed first occurrence.
    token = http_trim_ows(token);
    if (token.empty()) {
        return;
    }
    const bool wildcard_token = token == "*";
    if (!wildcard_token && !is_valid_http_header_name(token)) {
        throw std::invalid_argument("invalid HTTP Vary header token");
    }

    std::size_t value_size = token.size();
    constexpr auto max_value_size = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()) - 4;
    detail::validate_response_header_storage_size(4, value_size);
    if ((known_header_bits_ & detail::response_header_vary) != 0) {
        for (const auto& header_value : *this) {
            if (detail::http_response_header_access::known_bit(header_value) != detail::response_header_vary) {
                continue;
            }
            const auto value = header_value.value();
            if (detail::http_has_token(value, "*") || (!wildcard_token && detail::http_has_token(value, token))) {
                return;
            }
            if (!wildcard_token && !http_trim_ows(value).empty()) {
                if (value.size() > max_value_size - value_size || max_value_size - value_size - value.size() < 2) {
                    throw std::length_error("HTTP response header is too large");
                }
                value_size += value.size() + 2;
            }
        }
    }

    if (wildcard_token) {
        header("Vary", "*");
        return;
    }
    if (value_size == token.size()) {
        header("Vary", token);
        return;
    }

    // Build independently: token may borrow a field that publication retires.
    auto prepared = make_uninitialized_header("Vary", value_size, detail::response_header_vary);
    auto* output = detail::http_response_header_access::value_begin(prepared);
    for (const auto& header_value : *this) {
        if (detail::http_response_header_access::known_bit(header_value) != detail::response_header_vary) {
            continue;
        }
        const auto value = header_value.value();
        if (!http_trim_ows(value).empty()) {
            std::memcpy(output, value.data(), value.size());
            output += value.size();
            *output++ = ',';
            *output++ = ' ';
        }
    }
    std::memcpy(output, token.data(), token.size());
    // Existing lines and token are validated; comma-joining preserves validity.
    publish_header(prepared);
}

}  // namespace ruvia
