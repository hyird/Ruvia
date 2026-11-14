#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "http_header.h"
#include "http_message.h"

namespace ruvia {

class http_origin_view;

struct http_cookie_jar_options final {
    std::size_t max_cookies_{256};
    std::size_t max_cookie_bytes_{std::size_t{32} * 1024};
};

// Sans-I/O RFC 6265 client cookie jar for a fixed origin. Validates Domain,
// Path, Secure, SameSite=None, Partitioned, and __Secure-/__Host- prefixes,
// caps Max-Age/Expires at 400 days, evicts expired entries, and enforces
// count and byte budgets.
class http_cookie_jar final {
public:
    explicit http_cookie_jar(const http_origin_view& origin, http_cookie_jar_options options = {});
    http_cookie_jar(http_scheme scheme, std::string_view host, http_cookie_jar_options options = {});

    void add_configured_cookie(std::string_view name, std::string_view value);

    bool absorb_set_cookie(std::string_view request_target, std::string_view set_cookie_value, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    void retain_response_cookies(std::string_view request_target, std::span<const http_header> headers, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
    void retain_response_cookies(std::string_view request_target, std::span<const http_header_view> headers, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    [[nodiscard]] std::string cookie_header(std::string_view request_target, bool include_received = true, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    template <typename fn_type>
    void visit_matching_cookies(std::string_view request_target, bool include_received, std::chrono::system_clock::time_point now, fn_type&& fn) {
        if (cookies_.empty()) {
            return;
        }
        if (next_expiry_.has_value() && now >= *next_expiry_) {
            discard_expired(now);
        }
        if (cookies_.empty()) {
            return;
        }
        const auto path = request_path_only(request_target);
        for (const auto& cookie : cookies_) {
            if (!cookie.configured_ && !include_received) {
                continue;
            }
            if (cookie.secure_ && scheme_ != http_scheme::https) {
                continue;
            }
            if (!cookie_path_matches(path, cookie.path_)) {
                continue;
            }
            fn(std::string_view(cookie.name_), std::string_view(cookie.value_));
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return cookies_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return cookies_.empty();
    }
    [[nodiscard]] std::size_t storage_bytes() const noexcept {
        return cookie_bytes_;
    }
    void clear() noexcept;

    [[nodiscard]] static bool cookie_domain_matches(std::string_view host, std::string_view domain) noexcept;
    [[nodiscard]] static bool cookie_path_matches(std::string_view request_path, std::string_view cookie_path) noexcept;
    [[nodiscard]] static std::string_view request_path_only(std::string_view target) noexcept;
    [[nodiscard]] static std::string_view default_cookie_path(std::string_view target) noexcept;

private:
    struct stored_cookie final {
        stored_cookie(std::string_view name, std::string_view value) :
            stored_cookie(name, value, "/", {}) {}
        stored_cookie(std::string_view name, std::string_view value, std::string_view path, std::string_view domain) :
            name_(name),
            value_(value),
            path_(path),
            domain_(domain) {}

        std::string name_;
        std::string value_;
        std::string path_;
        std::string domain_;
        std::optional<std::chrono::system_clock::time_point> expires_;
        bool secure_{false};
        bool partitioned_{false};
        bool configured_{true};
    };

    struct received_cookie final {
        std::string_view name_;
        std::string_view value_;
        std::string_view path_;
        std::string_view domain_;
        std::optional<std::chrono::system_clock::time_point> expires_;
        bool secure_{false};
        bool partitioned_{false};
        bool expired_{false};
    };

    [[nodiscard]] std::optional<received_cookie> accept_set_cookie_view(std::string_view request_target, std::string_view set_cookie_value, std::chrono::system_clock::time_point now) const;
    bool store_received_cookie(const received_cookie& cookie);
    void reserve_cookie_slot();
    void discard_expired(std::chrono::system_clock::time_point now);
    [[nodiscard]] static std::size_t entry_storage_bytes(std::string_view name, std::string_view value, std::string_view path, std::string_view domain) noexcept;
    [[nodiscard]] bool has_capacity(std::size_t replaced_bytes, std::size_t replacement_bytes, bool adding) const noexcept;
    void recompute_next_expiry() noexcept;

    std::string host_;
    http_scheme scheme_{http_scheme::http};
    std::size_t max_cookies_{256};
    std::size_t max_cookie_bytes_{std::size_t{32} * 1024};
    std::vector<stored_cookie> cookies_;
    std::optional<std::chrono::system_clock::time_point> next_expiry_{};
    std::size_t cookie_bytes_{0};
};

}  // namespace ruvia
