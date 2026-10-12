#include <cstdint>
#include <span>
#include <stdexcept>

#include "ruvia/http/http_base64.h"

#include "auth/jwt_primitives.h"

namespace ruvia::detail {

std::pmr::string jwt_base64_url_encode(std::string_view input, std::pmr::memory_resource* resource) {
    std::pmr::string out(pmr_resource_or_default(resource));
    out.resize(http_base64_encoded_size(input.size(), http_base64_padding::forbidden));
    http_base64_encode(out.data(),
        std::span(reinterpret_cast<const std::uint8_t*>(input.data()), input.size()),
        http_base64_charset::url_safe, http_base64_padding::forbidden);
    return out;
}

std::pmr::string jwt_base64_url_decode(std::string_view input, std::pmr::memory_resource* resource) {
    std::pmr::string out(pmr_resource_or_default(resource));
    out.resize(http_base64_decoded_size_bound(input.size()));
    const auto decoded_size = http_base64_decode(input,
        std::span(reinterpret_cast<std::uint8_t*>(out.data()), out.size()),
        http_base64_charset::url_safe, http_base64_padding::forbidden);
    if (!decoded_size) {
        throw std::invalid_argument("JWT base64url value is invalid");
    }
    out.resize(*decoded_size);
    return out;
}

}  // namespace ruvia::detail
