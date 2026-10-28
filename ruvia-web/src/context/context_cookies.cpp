#include <array>
#include <string_view>
#include <utility>

#include "ruvia/http/cookies.h"
#include "ruvia/web/context.h"

#include "auth/cookie_signature.h"
#include "context/context_response_state.h"

// Setting response cookies, including the two rules that make a cookie's name on
// the wire differ from the name the application used: a __Host-/__Secure- prefix
// becomes part of the name, and a signed cookie's MAC covers that wire name.

namespace ruvia {
namespace {

void write_cookie(detail::context_response_state& state_value, std::string_view name, std::string_view value,
    const cookie_options& options) {
    const set_cookie_plan plan(name, value, options);
    state_value.update_metadata([&plan](http_response& response) { response.set_cookie(plan); });
}

}  // namespace

void context::set_cookie(set_cookie_options options) {
    write_cookie(response_state(), options.name_.view(), options.value_.view(), options.attributes_);
}

void context::set_signed_cookie(set_signed_cookie_options options) {
    const auto name = options.name_.view();
    const auto value = options.value_.view();
    const auto secret = options.secret_.view();
    std::array<char, 128> wire_name_inline{};
    std::pmr::string wire_name_heap(pool());
    std::string_view wire_name = name;
    if (options.attributes_.prefix_) {
        const auto prefix = ruvia::http_cookie_prefix_text(*options.attributes_.prefix_);
        const auto wire_size = prefix.size() + name.size();
        if (wire_size <= wire_name_inline.size()) {
            prefix.copy(wire_name_inline.data(), prefix.size());
            name.copy(wire_name_inline.data() + prefix.size(), name.size());
            wire_name = std::string_view(wire_name_inline.data(), wire_size);
        } else {
            wire_name_heap.reserve(wire_size);
            wire_name_heap.append(prefix);
            wire_name_heap.append(name);
            wire_name = wire_name_heap;
        }
    }
    char signature[detail::cookie_signature_size];
    detail::write_cookie_signature(signature, secret, wire_name, value, pool());
    const auto signed_size = value.size() + 1U + detail::cookie_signature_size;
    std::array<char, 256> signed_inline{};
    std::pmr::string signed_heap(pool());
    std::string_view signed_view;
    if (signed_size <= signed_inline.size()) {
        if (!value.empty()) {
            value.copy(signed_inline.data(), value.size());
        }
        signed_inline[value.size()] = '.';
        std::char_traits<char>::copy(signed_inline.data() + value.size() + 1U, signature, sizeof(signature));
        signed_view = std::string_view(signed_inline.data(), signed_size);
    } else {
        signed_heap.reserve(signed_size);
        signed_heap.append(value);
        signed_heap.push_back('.');
        signed_heap.append(signature, sizeof(signature));
        signed_view = signed_heap;
    }
    write_cookie(response_state(), name, signed_view, options.attributes_);
}

void context::delete_cookie(delete_cookie_options options) {
    options.attributes_.max_age_.reset();
    options.attributes_.expires_ = cookie_deletion_expires;
    write_cookie(response_state(), options.name_.view(), "", options.attributes_);
}

}  // namespace ruvia
