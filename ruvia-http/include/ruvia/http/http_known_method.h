#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia {

enum class http_known_method : std::uint8_t {
    get,
    post,
    put,
    delete_value,
    patch,
    head,
    options,
    connect,
    unknown
};

// HTTP methods are an extensible, case-sensitive token space. http_known_method is
// only the framework's fixed semantic classification; it is never the wire value.
[[nodiscard]] http_known_method classify_http_method(std::string_view method) noexcept;
[[nodiscard]] std::string_view known_http_method_token(http_known_method method) noexcept;
[[nodiscard]] bool is_valid_http_method_token(std::string_view method) noexcept;

// RFC 9110 method properties over the exact, case-sensitive wire token. Unknown
// extension methods are conservatively neither safe nor idempotent; TRACE is
// recognized even though it is intentionally not a routable fixed enum member.
[[nodiscard]] bool is_http_method_safe(std::string_view method) noexcept;
[[nodiscard]] bool is_http_method_idempotent(std::string_view method) noexcept;

}  // namespace ruvia
