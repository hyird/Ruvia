#include <string_view>
#include <utility>

#include "ruvia/http/cookies.h"
#include "ruvia/web/context.h"

#include "auth/cookie_signature.h"
#include "context/context_response_state.h"
#include "util/inline_text_concat.h"

// Setting response cookies, including the two rules that make a cookie's name on
// the wire differ from the name the application used: a __Host-/__Secure- prefix
// becomes part of the name, and a signed cookie's MAC covers that wire name.

namespace ruvia {
namespace {

void write_cookie(http_response& response, std::string_view name, std::string_view value,
    const cookie_options& options) {
    const set_cookie_plan plan(name, value, options);
    response.set_cookie(plan);
}

}  // namespace

void context_response_editor::set_cookie(set_cookie_options options) {
    write_cookie(target(), options.name_.view(), options.value_.view(), options.attributes_);
}

void context_response_editor::set_signed_cookie(set_signed_cookie_options options) {
    const auto name = options.name_.view();
    const auto value = options.value_.view();
    const auto secret = options.secret_.view();
    const auto prefix = options.attributes_.prefix_
                            ? std::string_view(ruvia::http_cookie_prefix_text(*options.attributes_.prefix_))
                            : std::string_view{};
    const detail::inline_text_concat<128> wire_name({prefix, name}, owner_->pool());
    char signature[detail::cookie_signature_size];
    detail::write_cookie_signature(signature, secret, wire_name.view(), value, owner_->pool());
    const detail::inline_text_concat<256> signed_value(
        {value, ".", std::string_view(signature, sizeof(signature))}, owner_->pool());
    write_cookie(target(), name, signed_value.view(), options.attributes_);
}

void context_response_editor::delete_cookie(delete_cookie_options options) {
    options.attributes_.max_age_.reset();
    options.attributes_.expires_ = cookie_deletion_expires;
    write_cookie(target(), options.name_.view(), "", options.attributes_);
}

}  // namespace ruvia
