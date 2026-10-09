#include <chrono>
#include <string_view>
#include <utility>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_set_cookie_plan.h"
#include "ruvia/web/context.h"

#include "auth/cookie_signature.h"
#include "context/context_response_state.h"

// Setting response cookies, including the two rules that make a cookie's name on
// the wire differ from the name the application used: a __Host-/__Secure- prefix
// becomes part of the name, and a signed cookie's MAC covers that wire name.

namespace ruvia {
namespace {

// The name the client sends back in Cookie is the wire name: an enum prefix
// becomes part of the name at serialization. Request-side lookups and the MAC
// of a signed cookie must both use it; the bare name never reaches the client.
[[nodiscard]] std::string_view cookie_wire_name(
    std::pmr::string& storage, std::string_view name, const ruvia::cookie_options& options) {
    if (!options.prefix_) {
        return name;
    }
    const auto prefix = ruvia::http_cookie_prefix_text(*options.prefix_);
    storage.reserve(prefix.size() + name.size());
    storage.append(prefix.data(), prefix.size());
    storage.append(name.data(), name.size());
    return storage;
}

[[nodiscard]] std::pmr::string compose_signed_cookie_value(std::pmr::memory_resource* resource,
    std::string_view name, std::string_view value, std::string_view secret) {
    std::pmr::string signed_value(resource);
    signed_value.reserve(value.size() + 1 + detail::cookie_signature_size);
    if (!value.empty()) {
        signed_value.append(value.data(), value.size());
    }
    signed_value.push_back('.');
    char signature[detail::cookie_signature_size];
    detail::write_cookie_signature(signature, secret, name, value);
    signed_value.append(signature, sizeof(signature));
    return signed_value;
}

void write_cookie(http_response& response, std::string_view name, std::string_view value,
    const cookie_options& options) {
    const set_cookie_plan plan(name, value, options);
    response.set_cookie(plan);
}

}  // namespace

void context::set_cookie(set_cookie_options options) {
    write_cookie(response_state().active_response(), options.name_.view(), options.value_.view(),
        options.attributes_);
}

void context::set_signed_cookie(set_signed_cookie_options options) {
    std::pmr::string wire_name(pool());
    const auto name = options.name_.view();
    const auto value = options.value_.view();
    const auto secret = options.secret_.view();
    write_cookie(response_state().active_response(), name,
        compose_signed_cookie_value(
            pool(), cookie_wire_name(wire_name, name, options.attributes_), value, secret),
        options.attributes_);
}

void context::delete_cookie(delete_cookie_options options) {
    options.attributes_.max_age_ = std::chrono::seconds(0);
    write_cookie(response_state().active_response(), options.name_.view(), "", options.attributes_);
}

}  // namespace ruvia
