#include "ruvia/web/csrf.h"

#include <array>
#include <cstddef>
#include <stdexcept>

#include <openssl/rand.h>

#include "ruvia/core/hex.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/detail/util/registration_resource.h"

#include "http/secure_token.h"

namespace ruvia::detail {

secure_token_result generate_secure_token(std::span<char> buffer) noexcept {
    constexpr std::size_t random_bytes = 24;  // 24 bytes -> 48 hex characters
    if (buffer.size() < random_bytes * 2) {
        return secure_token_result::make_failure();
    }
    unsigned char raw[random_bytes];
    if (RAND_bytes_ex(nullptr, raw, random_bytes, 0) != 1) {
        return secure_token_result::make_failure();
    }
    for (std::size_t i = 0; i < random_bytes; ++i) {
        buffer[i * 2] = ::ruvia::lower_hex_digit(raw[i] >> 4);
        buffer[i * 2 + 1] = ::ruvia::lower_hex_digit(raw[i]);
    }
    return secure_token_result::make_ready(std::string_view(buffer.data(), random_bytes * 2));
}

}  // namespace ruvia::detail

namespace ruvia {

csrf_protection::config_storage_type::validated_config_type csrf_protection::config_storage_type::validate(
    const csrf_protection_config& source_value) {
    if (!is_valid_http_header_name(source_value.cookie_name_)) {
        throw std::invalid_argument("CSRF cookie name must be a valid HTTP token");
    }
    if (!is_valid_http_header_name(source_value.header_name_)) {
        throw std::invalid_argument("CSRF header name must be a valid HTTP field name");
    }
    return validated_config_type{.source_ = &source_value};
}

csrf_protection::config_storage_type::config_storage_type(
    const csrf_protection_config& source_value, std::pmr::memory_resource* resource)
    : config_storage_type(validate(source_value), resource) {}

csrf_protection::config_storage_type::config_storage_type(
    validated_config_type validated, std::pmr::memory_resource* resource)
    : cookie_name_(validated.source_->cookie_name_, resource),
      header_name_(validated.source_->header_name_, resource) {}

csrf_protection::csrf_protection()
    : csrf_protection(csrf_protection_config{}) {}

csrf_protection::csrf_protection(const csrf_protection_config& config)
    : config_(config, detail::registration_resource()) {}

task<void> csrf_protection::handle(context& c, next& next_value) {
    const auto method = c.req().known_method();
    const bool safe = method == http_known_method::get || method == http_known_method::head ||
                      method == http_known_method::options;
    const auto cookie = c.req().cookie(config_.cookie_name_);
    if (!safe) {
        const auto header_value = c.req().header(config_.header_name_);
        if (!cookie || cookie->empty() || !header_value || header_value->empty() ||
            !detail::csrf_tokens_equal(*cookie, *header_value)) {
            c.respond(c.error({.status_ = ruvia::http_status::forbidden,
                .code_ = "csrf_token_mismatch",
                .message_ = "CSRF token missing or invalid"}));
            co_return;
        }
    } else if (!cookie || cookie->empty()) {
        // Reseed on an absent OR empty cookie. The unsafe path above already
        // rejects an empty cookie as invalid, so if the safe path only reissued
        // when the cookie was fully absent, a present-but-empty "XSRF-TOKEN="
        // would never be repaired: every safe request would leave it empty and
        // every unsafe request would 403 on it -- a permanent wedge. Treating
        // absent and empty identically here keeps the issue and validation sides
        // of the double-submit symmetric.
        std::array<char, 64> buffer;
        const auto token_result = detail::generate_secure_token(buffer);
        const auto* token = token_result.ready();
        if (token == nullptr) {
            c.respond(c.error({.status_ = ruvia::http_status::internal_server_error,
                .code_ = "secure_random_failed",
                .message_ = "secure token generation failed"}));
            co_return;
        }
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
        c.set_cookie({.name_ = config_.cookie_name_, .value_ = token->value(), .attributes_ = options});
    }
    co_await next_value();
}

}  // namespace ruvia
