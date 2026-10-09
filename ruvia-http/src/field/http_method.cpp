#include <algorithm>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_known_method.h"

// The method vocabulary: recognising the standard tokens, spelling them back,
// validating an unknown one, and the two properties -- safe and idempotent --
// that decide what a recipient may do with a request (RFC 9110 section 9.2).

namespace ruvia {

http_known_method classify_http_method(std::string_view method) noexcept {
    switch (method.size()) {
        case 3:
            if (method == "GET") {
                return http_known_method::get;
            }
            if (method == "PUT") {
                return http_known_method::put;
            }
            break;
        case 4:
            if (method == "POST") {
                return http_known_method::post;
            }
            if (method == "HEAD") {
                return http_known_method::head;
            }
            break;
        case 5:
            if (method == "PATCH") {
                return http_known_method::patch;
            }
            break;
        case 6:
            if (method == "DELETE") {
                return http_known_method::delete_value;
            }
            break;
        case 7:
            if (method == "OPTIONS") {
                return http_known_method::options;
            }
            if (method == "CONNECT") {
                return http_known_method::connect;
            }
            break;
        default:
            break;
    }
    return http_known_method::unknown;
}

std::string_view known_http_method_token(http_known_method method) noexcept {
    switch (method) {
        case http_known_method::get:
            return "GET";
        case http_known_method::post:
            return "POST";
        case http_known_method::put:
            return "PUT";
        case http_known_method::delete_value:
            return "DELETE";
        case http_known_method::patch:
            return "PATCH";
        case http_known_method::head:
            return "HEAD";
        case http_known_method::options:
            return "OPTIONS";
        case http_known_method::connect:
            return "CONNECT";
        case http_known_method::unknown:
        default:
            return {};
    }
}

bool is_valid_http_method_token(std::string_view method) noexcept {
    if (method.empty()) {
        return false;
    }
    return std::ranges::all_of(method,
        [](char ch) noexcept { return detail::is_http_token_char(static_cast<unsigned char>(ch)); });
}

bool is_http_method_safe(std::string_view method) noexcept {
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "TRACE";
}

bool is_http_method_idempotent(std::string_view method) noexcept {
    return is_http_method_safe(method) || method == "PUT" || method == "DELETE";
}

}  // namespace ruvia
