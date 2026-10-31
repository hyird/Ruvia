#include "ruvia/web/csrf.h"

#include <array>
#include <cstddef>
#include <stdexcept>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/util/registration_resource.h"
#include "ruvia/web/error.h"

#include "http/secure_token.h"
#include "tls/tls_random.h"
#include "util/constant_time.h"

namespace ruvia::detail {

std::string_view generate_secure_token(std::span<char, secure_token_size> buffer) {
    std::array<unsigned char, secure_token_size / 2> raw;
    if (!fill_random_bytes(raw)) {
        throw std::runtime_error("secure random generation failed");
    }
    http_encode_lower_hex(buffer.data(), std::span<const unsigned char>(raw));
    return std::string_view(buffer.data(), buffer.size());
}
}  // namespace ruvia::detail

namespace ruvia {

const csrf_protection_config& csrf_protection::validate(const csrf_protection_config& source_value) {
    if (!is_valid_http_header_name(source_value.cookie_name_)) {
        throw std::invalid_argument("CSRF cookie name must be a valid HTTP token");
    }
    if (!is_valid_http_header_name(source_value.header_name_)) {
        throw std::invalid_argument("CSRF header name must be a valid HTTP field name");
    }
    return source_value;
}

csrf_protection::csrf_protection(
    const csrf_protection_config& validated, std::pmr::memory_resource* resource)
    : cookie_name_(validated.cookie_name_, resource),
      header_name_(validated.header_name_, resource) {}

csrf_protection::csrf_protection()
    : csrf_protection(csrf_protection_config{}) {}

csrf_protection::csrf_protection(const csrf_protection_config& config)
    : csrf_protection(validate(config), detail::registration_resource()) {}

task<void> csrf_protection::handle(context& c, next& next_value) {
    const auto method = c.req().known_method();
    const bool safe = method == http_known_method::get || method == http_known_method::head ||
                      method == http_known_method::options;
    const auto cookie = c.req().cookie(cookie_name_);
    if (!safe) {
        const auto header_value = c.req().header(header_name_);
        if (!cookie || cookie->empty() || !header_value || header_value->empty() ||
            !detail::constant_time_bytes_equal(*cookie, *header_value)) {
            throw http_error({.status_ = ruvia::http_status::forbidden,
                .code_ = "csrf_token_mismatch",
                .message_ = "CSRF token missing or invalid"});
        }
    } else if (!cookie || cookie->empty()) {
        // Reseed on an absent OR empty cookie. The unsafe path above already
        // rejects an empty cookie as invalid, so if the safe path only reissued
        // when the cookie was fully absent, a present-but-empty "XSRF-TOKEN="
        // would never be repaired: every safe request would leave it empty and
        // every unsafe request would 403 on it -- a permanent wedge. Treating
        // absent and empty identically here keeps the issue and validation sides
        // of the double-submit symmetric.
        std::array<char, detail::secure_token_size> buffer;
        const auto token = detail::generate_secure_token(buffer);
        const auto connection = c.conn();
        // Secure follows the client's scheme, including TLS a trusted proxy
        // terminated. tls() is this hop only; using it would omit Secure behind
        // a plaintext reverse proxy and leave the token readable on HTTP.
        const cookie_options options{
            .path_ = "/",
            .same_site_ = cookie_same_site::lax,
            .secure_ = connection.scheme() == http_scheme::https ? cookie_attribute_policy::emit
                                                                 : cookie_attribute_policy::omit,
        };
        c.set_cookie({.name_ = cookie_name_, .value_ = token, .attributes_ = options});
    }
    co_await next_value();
}

}  // namespace ruvia
